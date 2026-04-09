## Why

Phase B 中 `RaBitQEncoder::Encode` 是当前构建阶段最大的瓶颈。在 Deep1M（`--bits 4`，N=1M，dim=128）上实测占 **50.9 秒 / 90.5 秒（56.2%）**。

根因：ExRaBitQ 的 M-bit 量化路径对**每个向量**都执行一次完整的 `best_rescale_factor` 栅格搜索（`src/rabitq/rabitq_encoder.cpp:106-185`）：

```cpp
// 每个向量都重跑一遍：
double t_start = (max_code / 3.0) / max_o;         // 粗糙启发式
std::vector<int> cur_code(L);
std::priority_queue<...> pq;                        // 动态分配
while (!pq.empty()) { ... }                         // O(L log L) 栅格搜索
// 然后用 best_t 重新量化一次
```

每向量的热路径：
1. `std::vector<float> abs_rot(L)` + `std::vector<int> cur_code(L)` → 2 次 heap alloc
2. `std::priority_queue` → 动态堆
3. `while` 循环至多 `L × (2^ex_bits)` 次迭代，每次 `std::sqrt` + priority queue pop/push
4. 再遍历一次 L 做最终 re-quantize

**1M 向量 × L=128 × ex_bits=4 搜索空间 ≈ 50 秒**

官方 RaBitQ-Library 提供了**完全避免栅格搜索**的快速路径（`thrid-party/RaBitQ-Library/include/rabitqlib/quantization/rabitq_impl.hpp:263-403`）：

```cpp
// 预计算一次（per (dim, ex_bits)）：
constexpr std::array<float, 9> kTightStart = {0, 0.15, 0.20, 0.52, 0.59, 0.71, 0.75, 0.77, 0.81};
double t_const = get_const_scaling_factors(dim, ex_bits);  // 用 100 个 random gaussian 采样 best_rescale_factor 的均值

// 每个向量只做一次 O(L) 扫描（无搜索、无堆、无 priority queue）：
faster_quantize_ex(abs_rot, code, dim, ex_bits, t_const);
```

理论依据：`best_rescale_factor` 的最优 t 在固定 (dim, ex_bits) 下分布集中于 `t_const` 附近，直接用均值作为全局缩放因子，编码误差代价极小（官方库的 benchmark 验证），而速度提升 10-50x。

**当前 Deep1M 基线（performance governor，稳定态）：**
- Phase B 总耗时 ~90.5 s（其中 Encode ~50.9 s）
- recall@10 = 0.9685
- Phase D 平均延迟 ~1.0 ms

预期优化后：
- Phase B Encode 50.9 s → **1-5 s**（10-50× 加速）
- Phase B 总耗时 ~90 s → ~40 s
- recall@10、lamhat、Phase D 延迟均不变（编码精度等价）

## What Changes

- **`include/vdb/rabitq/rabitq_encoder.h`**：新增 `RaBitQConfig` 结构（缓存每个 (dim, ex_bits) 组合的 `t_const`）；新增 `FastQuantize` 快速路径接口
- **`src/rabitq/rabitq_encoder.cpp`**：
  - 新增 `kTightStart` 常量表（对齐官方 `kTightStart[1..8]`）
  - 新增 `ComputeTConst(dim, ex_bits)`：用 100 个随机高斯向量采样计算全局 `t_const`
  - 修改 `Encode()`：在 `bits > 1` 分支中，用 `FastQuantizeEx(abs_rot, t_const)` 替代原 priority queue 栅格搜索
  - 保留原栅格搜索路径作为 `EncodeSlow()`（可通过构造参数或编译宏开启，用于正确性对比和 fallback）
  - 在构造函数中 eager 计算 `t_const` 并缓存在成员变量
- **`benchmarks/bench_vector_search.cpp`**：无需修改（透明加速）

## Capabilities

### New Capabilities

- `rabitq-encoder-fast-path`: M-bit ExRaBitQ 编码的快速量化路径，基于 `t_const` 预计算消除 per-vector 栅格搜索

### Modified Capabilities

- `rabitq-encoder`: 默认使用快速路径；保留 slow path 作为参考实现

## Impact

- **代码路径**：
  - `include/vdb/rabitq/rabitq_encoder.h` — 新增 `RaBitQConfig`、`kTightStart`、`t_const_` 成员
  - `src/rabitq/rabitq_encoder.cpp` — 新增 `ComputeTConst`、`FastQuantizeEx`；`Encode()` 切换到快速路径
  - `benchmarks/bench_vector_search.cpp` — 无改动
- **内存**：+`sizeof(double)` per encoder（`t_const_` 成员），零额外堆分配
- **兼容性**：非破坏性变更；原 slow path 保留，可通过开关切换用于数值对比验证
- **依赖**：无新外部依赖；`ComputeTConst` 需要一个简单的高斯采样（可复用 `<random>` std lib）

## Non-Goals

- 不修改 1-bit RaBitQ 编码路径（已经足够快）
- 不修改 `RaBitQCode` 结构或 `FastScan` 打包布局
- 不改变 Estimator / Phase D 查询路径
- 不改变 `bits=1`（纯 1-bit）时的行为（该路径不走 ex_bits 量化）
- 不引入 hi-acc FastScan（另一个 change 范围）
- 不直接 include 官方 lib 的 header（避免引入 Eigen 依赖）；手工移植关键函数
