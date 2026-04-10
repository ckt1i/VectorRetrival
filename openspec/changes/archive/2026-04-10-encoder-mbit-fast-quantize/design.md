## Context

当前 `RaBitQEncoder::Encode`（`src/rabitq/rabitq_encoder.cpp`）对每个向量的 ExRaBitQ 量化路径：

```
每个向量 v (1M 次):
  ├─ residual = v - centroid                           // O(L)
  ├─ rotated = P × residual                            // O(L log L) via FWHT (已优化)
  ├─ abs_rot = |rotated|                               // heap alloc O(L)
  ├─ max_o = max(abs_rot)                              // O(L)
  ├─ cur_code = int[L]                                 // heap alloc O(L)
  ├─ priority_queue<...> pq                            // 动态堆
  ├─ 初始化 cur_code + 填充 pq                          // O(L)
  ├─ while (pq non-empty):                             // ★ 主瓶颈 ★
  │    pop_top → update cur_code → compute IP bound    //   O(L × 2^ex_bits × log L) 最坏
  └─ 遍历 abs_rot re-quantize with best_t               // O(L)
```

Deep1M 基线实测：**50.9 秒 / 1M vectors ≈ 51 μs/vector**，其中绝大部分是 priority queue 栅格搜索。

官方 RaBitQ-Library (`rabitq_impl.hpp:263-403`) 的设计洞察：

> 对于固定的 (dim, ex_bits)，`best_rescale_factor` 在随机高斯输入上的分布高度集中。与其**每次**调用 priority queue 搜索，不如**一次性**采样 100 个随机向量、计算均值 `t_const`，所有向量直接用 `t_const` 量化。

这是 "构建时一次预计算 + 查询时 O(L) 快速路径" 的经典套路。

## Goals / Non-Goals

**Goals:**
- 消除 `Encode()` 中 per-vector 的 priority queue 栅格搜索
- Deep1M Phase B Encode 从 50.9s 降至 1-5s（预期 10-50× 加速）
- 保持 ExRaBitQ 编码精度 ≈ 不变（recall@10 波动 ≤ 0.002）
- 保持 Phase D 查询 latency 与 recall 不变
- 保留原 slow path 作为数值对比基准

**Non-Goals:**
- 不修改 1-bit sign encoding 路径（已经是 SIMD 最优）
- 不改变 `RaBitQCode` 结构或 FastScan layout
- 不修改 `RaBitQEstimator`
- 不合并 rotation / FWHT 优化（已在独立 change 中）
- 不直接 include 官方 lib 的 header（Eigen 依赖过重）

## Decisions

### Decision 1: 引入 `RaBitQConfig` 缓存结构

**动机**：`t_const` 只依赖于 `(dim, ex_bits)`，应在 encoder 构造时一次性计算并缓存。

**方案**：在 `RaBitQEncoder` 中直接添加成员变量：

```cpp
// include/vdb/rabitq/rabitq_encoder.h
class RaBitQEncoder {
public:
    RaBitQEncoder(uint32_t dim, const RotationMatrix& rotation,
                  uint32_t bits = 1, uint64_t t_const_seed = 42);
    // ...
private:
    // NEW members:
    double t_const_ = 0.0;   // 预计算的缩放因子；仅在 bits > 1 时有效
    int max_code_ = 0;       // (1 << ex_bits) - 1，预计算
    // ... existing members ...
};
```

**为什么不单独做 `RaBitQConfig` struct**：当前 encoder 只有一份配置，不需要额外结构；直接放在 encoder 内最简洁。将来如果需要多配置共享，再抽出。

### Decision 2: `ComputeTConst` 的采样与随机数来源

**官方实现**（`rabitq_impl.hpp:363-377`）：

```cpp
inline double get_const_scaling_factors(size_t dim, size_t ex_bits) {
    constexpr long kConstNum = 100;
    RowMajorArray<double> rand = random_gaussian_matrix<double>(kConstNum, dim);
    rand = rand.rowwise().normalized().abs();
    double sum = 0;
    for (long j = 0; j < kConstNum; ++j)
        sum += best_rescale_factor(&rand(j, 0), dim, ex_bits);
    return sum / kConstNum;
}
```

**我们的移植**：避免引入 Eigen，改用 `std::mt19937` + `std::normal_distribution`：

```cpp
// src/rabitq/rabitq_encoder.cpp
static double ComputeTConst(uint32_t dim, uint32_t ex_bits, uint64_t seed) {
    constexpr int kSamples = 100;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    std::vector<double> v(dim);
    double sum_t = 0.0;
    for (int s = 0; s < kSamples; ++s) {
        // 1. 采样随机高斯向量
        for (uint32_t i = 0; i < dim; ++i) v[i] = gauss(rng);
        // 2. normalize + abs
        double norm = 0;
        for (double x : v) norm += x * x;
        norm = std::sqrt(norm);
        for (double& x : v) x = std::abs(x / norm);
        // 3. 对此样本跑一次 best_rescale_factor（slow path 复用）
        sum_t += BestRescaleFactorSlow(v.data(), dim, ex_bits);
    }
    return sum_t / kSamples;
}
```

**种子策略**：
- 构造函数接受 `t_const_seed`（默认 42），保证可重现
- 100 个样本足够稳定（官方验证）

### Decision 3: `FastQuantizeEx` 实现（Encode 的快速路径）

直接对应官方 `faster_quantize_ex`（`rabitq_impl.hpp:380-403`）：

```cpp
// 对每个向量：替代 Encode() 中 line 106-185 的所有代码
static float FastQuantizeEx(const float* abs_rot, uint32_t dim, int max_code,
                             double t_const,
                             uint8_t* out_ex_code /* length dim */) {
    constexpr double kEps = 1e-5;
    double ipnorm = 0.0;
    for (uint32_t i = 0; i < dim; ++i) {
        int c = static_cast<int>(t_const * abs_rot[i] + kEps);
        if (c > max_code) c = max_code;
        out_ex_code[i] = static_cast<uint8_t>(c);
        ipnorm += (c + 0.5) * abs_rot[i];
    }
    // xipnorm = 1 / ipnorm
    return (ipnorm > 1e-30) ? static_cast<float>(1.0 / ipnorm) : 1.0f;
}
```

**关键点**：
- 无 priority_queue，无 heap alloc，纯 O(L) 扫描
- 与 slow path 产出完全相同的 `ex_code` 和 `xipnorm` 字段结构
- 只是用 `t_const` 代替 `best_t`

### Decision 4: Slow path 保留与启用机制

为了方便数值对比和回退，保留原 slow path 但移入私有方法：

```cpp
// 成员方法
RaBitQCode Encode(const float* v, const float* centroid) const;      // 快速（默认）
RaBitQCode EncodeSlow(const float* v, const float* centroid) const;  // 原栅格搜索
```

**启用机制**：
- 默认 `Encode()` 走快速路径
- 单元测试调用 `EncodeSlow()` 对若干样本验证 `ex_code` 差异小（汉明距离统计 + recall 验证）
- 不加运行时开关（避免分支开销 / 测试维度爆炸）；需要对比时改代码或写临时 benchmark

### Decision 5: `kTightStart` 的角色

观察：`kTightStart` 在官方 slow path (`best_rescale_factor`) 中用于**剪枝起始 t**，但在 fast path (`faster_quantize_ex`) 中**完全没用到**。

**结论**：`kTightStart` 只在我们保留的 `EncodeSlow` 中使用（移植到 `BestRescaleFactorSlow` 函数，以便 `ComputeTConst` 采样 100 个高斯向量时也能利用官方的剪枝起点，加快 `t_const` 计算）。

```cpp
// src/rabitq/rabitq_encoder.cpp
static constexpr double kTightStart[9] = {
    0.0, 0.15, 0.20, 0.52, 0.59, 0.71, 0.75, 0.77, 0.81
};

static double BestRescaleFactorSlow(const double* abs_rot, uint32_t dim, uint32_t ex_bits) {
    // 官方 best_rescale_factor 的 double 版本
    // 使用 kTightStart[ex_bits] 作为 t_start 的剪枝起点
    // （当前 encoder 用 max_code/3，是更粗糙的启发式）
}
```

### Decision 6: 数值精度与 recall 保证

**风险**：t_const 是全局均值，对极端分布的向量可能次优，理论上会引入小量编码误差。

**缓解**：
1. 官方 lib benchmark 已验证此方法在 ex_bits=1..8 上与 slow path 精度差 < 0.1% recall
2. 我们在 tasks.md 中加入强制数值校验：COCO 和 Deep1M 双数据集 recall@10 波动 ≤ 0.002（等同于 CRC change 的验收标准）
3. 如果观察到 recall drop > 0.002，回退策略：
   - Option A: 每次 Encode 前对 `abs_rot` 检查分布偏度，偏度超阈值时走 slow path
   - Option B: 增加 `kSamples` 或使用 dataset-specific `t_const`（在 Phase B 开始时用 base data 前 100 个向量采样）

Tasks 中 4.x 的验证步骤会决定是否需要 Option A/B。

### Decision 7: `Encode()` 函数内部结构

最终的 `Encode` 快速路径简化为：

```cpp
RaBitQCode RaBitQEncoder::Encode(const float* v, const float* centroid) const {
    RaBitQCode result;
    // ---- 1-bit sign encoding（不变）----
    ComputeResidual(v, centroid, residual);
    rotation_.Apply(residual, rotated);
    // ... pack sign bits ...

    // ---- ExRaBitQ fast quantize（替换原 lattice search）----
    if (bits_ > 1) {
        for (size_t i = 0; i < L; ++i) abs_rot[i] = std::abs(rotated[i]);
        result.ex_code.resize(L);
        result.ex_sign.resize(L);
        result.xipnorm = FastQuantizeEx(abs_rot.data(), L, max_code_, t_const_,
                                         result.ex_code.data());
        for (size_t i = 0; i < L; ++i) result.ex_sign[i] = (rotated[i] >= 0.0f);
    }

    result.sum_x = simd::PopcountTotal(result.code.data(), wpp);
    return result;
}
```

**注意**：
- `abs_rot` 仍需一次 alloc（可后续提升到 thread_local 或 scratch pool，次级优化）
- `ex_sign` 填充保持原样（bits 不变，逻辑 trivial）
- `xipnorm` 计算与官方一致

## Risks / Trade-offs

- **[编码精度]** 全局 `t_const` 对病态输入（如极度偏斜分布）可能产生 <1% 的 xipnorm 误差。由数值验证 (Decision 6) 把关。
- **[t_const 采样随机性]** 100 高斯样本的均值有 stochastic variance。通过固定 seed 保证可重现；100 样本经官方验证足够稳定。
- **[代码双路径维护]** Slow path 保留为参考实现，维护成本小，但需要在 tests 中保持两者的 API 对称。
- **[ABI 兼容]** `RaBitQEncoder` 构造签名新增默认参数 `t_const_seed`，向后兼容；如果 header-only 测试 snapshot 依赖精确成员布局可能需同步更新。
- **[与其他 changes 的协同]** 本 change 不触碰 Estimator / FastScan / CRC 路径，与 `precompute-rotated-centroids`、`simd-query-hotpath-opt`、`crc-calibration-fast-path` 完全正交，可并行推进。

## 预期收益

| 指标              | Deep1M 基线 | 预期优化后 | 改善    |
|------------------|-----------|----------|--------|
| Phase B Encode   | 50.9 s    | 1–5 s    | 10–50× |
| Phase B 总耗时    | 90.5 s    | ~40 s    | 2.2×   |
| recall@10        | 0.9685    | ≥ 0.9665 | 等价    |
| Phase D avg lat  | ~1.0 ms   | 不变      | –      |

（COCO 100K 上收益占比略小，因 N=100K 绝对时间本就较短；但加速比应接近。）

## 开放问题

- `t_const` 是否需要针对每个数据集重新采样？
  - 当前决定：**不**，使用 (dim, ex_bits) 作为唯一键；如 Decision 6 验证失败再考虑 dataset-specific 方案
- `abs_rot` 临时缓冲是否需要池化？
  - 初版不做；若 profile 显示仍有显著 alloc 开销再优化
- 未来若引入 8-bit 以上 ex_bits，`kTightStart` 表需扩展（目前 1..8 已覆盖常见场景）
