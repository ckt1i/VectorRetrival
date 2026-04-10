## Why

Phase B 中两个 ε 校准循环（`ε_ip` 和 `ε_ip_fs`）在 Deep1M 上实测合计消耗约 **35 秒 / 90.5 秒（38.7%）**，是仅次于 Encode 的第二大瓶颈。

**Deep1M Phase B 时序分解（实测）：**
```
load centroids+assignments    37.6 ms
Encode N vectors          50,889.1 ms   ← change A 处理
ε_ip calibration loop     29,015.5 ms   ← 本 change 处理 (#1)
FastScan block packing       481.4 ms
d_k calibration            4,260.1 ms
ε_ip_fs calibration        5,831.1 ms   ← 本 change 处理 (#2)
────────────────────────────────────────
Build complete            90,480.6 ms
```

### 瓶颈 #1：ε_ip 校准中的标量 bit×float 循环（29 s）

`benchmarks/bench_vector_search.cpp:440-444`：

```cpp
float dot = 0.0f;
for (size_t d = 0; d < dim; ++d) {
    int bit = (code.code[d / 64] >> (d % 64)) & 1;
    dot += pq.rotated[d] * (2.0f * bit - 1.0f);
}
float ip_accurate = dot * inv_sqrt_dim;
```

每次调用计算「全精度 query × 1-bit 符号 code」的内积，用于与 popcount 估计量（`ip_hat`）对比得到 ε_ip。

**调用量级（Deep1M, nlist=4096, samples_for_eps=100）：**
```
4096 clusters × 100 samples/cluster × ~250 members/cluster ≈ 1.02 × 10⁸ 次调用
每次: dim=128 次 scalar 位提取 + fmadd
总 scalar ops ≈ 1.3 × 10¹⁰
```

这是**纯标量**循环，既没有 SIMD 也没有并行化。

### 瓶颈 #2：两个校准循环都是 embarrassingly parallel（but 单线程）

外层 `for (uint32_t k = 0; k < nlist; ++k)` 对每个 cluster 独立处理。
- ε_ip 循环（`bench_vector_search.cpp:416-449`）
- ε_ip_fs 循环（`bench_vector_search.cpp:541-586`）

两者都写入独立的结果 vector（`ip_errors` 和 `fs_normalized_errors`），天然可以并行，但当前全部在主线程串行执行。

### 瓶颈 #3：samples_for_eps 偏保守

`samples_for_eps = 100`（`bench_vector_search.cpp:412`）意味着每个 cluster 抽 100 个 query，每个 query 对全体成员做 all-pairs。实际 ε 收敛只需更少样本（P-percentile 统计量的样本需求远低于 100 × 250）。但这是次级调优，本 change 不激进压低。

---

**当前 Deep1M 基线（performance governor，稳定态）：**
- Phase B 总耗时 90.5 s
- ε_ip 校准 ~29.0 s
- ε_ip_fs 校准 ~5.8 s
- recall@10 = 0.9685
- lamhat = 0.132548

**预期优化后：**
- ε_ip 校准 29.0 s → **3–6 s**（SIMD 5× + OMP N×）
- ε_ip_fs 校准 5.8 s → **1–2 s**（OMP N×）
- Phase B 总耗时（与 change A 联动后）从 ~40 s 再降至 ~30 s
- eps_ip / eps_ip_fs / lamhat / recall 完全不变（数值等价）

## What Changes

- **新增 SIMD kernel**：`include/vdb/simd/signed_dot.h` / `src/simd/signed_dot.cpp`
  - `float SignedDotFromBits(const float* q, const uint64_t* sign_bits, uint32_t dim)`
  - AVX-512 实现：每 16 float 一个 vector load，从 bit mask 构造 ±1.0f 向量（或用 `_mm512_mask_sub_ps` 翻转符号），fmadd 累加
  - 标量 fallback（确保正确性 + 对齐测试）
- **修改 `benchmarks/bench_vector_search.cpp`**：
  - ε_ip 循环（line 440-444）：替换为 `simd::SignedDotFromBits(pq.rotated.data(), code.code.data(), dim)`
  - ε_ip 外层循环：添加 `#pragma omp parallel for`，使用 thread-local `ip_errors` vector，最后合并
  - ε_ip_fs 外层循环：同样添加 OMP + thread-local vector + 合并
- **单元测试**：`tests/simd/test_signed_dot.cpp` 验证 SIMD 与标量版本在 dim=96/128/512 上 bit-exact（容差 < 1e-5）

## Capabilities

### New Capabilities

- `simd-signed-dot`: AVX-512 kernel 计算「全精度向量 × 1-bit 符号 code」内积
- `parallel-eps-calibration`: OMP 并行化 ε_ip 和 ε_ip_fs 校准外层 cluster 循环

### Modified Capabilities

- `eps-ip-calibration`: 使用 SIMD + OMP，消除 scalar 瓶颈
- `eps-ip-fs-calibration`: OMP 并行化

## Impact

- **代码路径**：
  - `include/vdb/simd/signed_dot.h` — 新增 SIMD kernel 声明
  - `src/simd/signed_dot.cpp` — AVX-512 + scalar 实现
  - `tests/simd/test_signed_dot.cpp` — 单元测试
  - `benchmarks/bench_vector_search.cpp` — 替换 scalar 循环 + 添加 OMP pragma
  - `CMakeLists.txt` 或 `src/simd/CMakeLists.txt` — 注册新源文件
- **依赖**：
  - 无新外部依赖
  - OpenMP（项目已在其他路径使用 `omp.h`；需确认 CMake 中 OMP 已启用）
- **内存**：per-thread `ip_errors` / `fs_normalized_errors` vector（合并前），总内存 ≈ N 线程 × 每线程样本数 × 4B，可忽略
- **兼容性**：
  - 数值：SignedDotFromBits 与原标量循环 bit-exact（同样的加法顺序或数值等价的重排，测试保证 ≤1e-5 相对误差）
  - ε_ip / ε_ip_fs / lamhat / recall 不变
  - OMP 非 deterministic 顺序不影响最终 P-percentile 统计量（sort 后一致）

## Non-Goals

- 不修改 RaBitQ 编码路径（由 `encoder-mbit-fast-quantize` 处理）
- 不修改 `RaBitQEstimator::PrepareQuery` / `EstimateDistanceFastScan`
- 不修改 `samples_for_eps` 常量（100 保守值保留；次级调优不在本 change 范围）
- 不修改 CRC 校准路径（由 `crc-calibration-fast-path` 处理）
- 不重构两个 ε 校准循环为公共函数（保留 ad-hoc 结构，修改面小）
- 不引入 dataset-specific 采样策略
- 不并行化 `d_k calibration`（`CalibrateDistanceThreshold` 在独立函数中，4.3 s 不是首要瓶颈；未来可单独处理）
