## Why

经过两轮优化（`encoder-mbit-fast-quantize` + `eps-calibration-simd`），Deep1M Phase B 已从 **90.5 s 降至 21.9 s（4.1×）**。当前剩余瓶颈分析：

```
Phase B 当前时序分解（Deep1M, bits=4）：
  eps 循环（SIMD+OMP）   ~10,000 ms   ← 已优化
  d_k 校准               ~4,260 ms    ← 本 change 处理 (#1)
  CRC 校准               ~4,196 ms    ← 本 change 处理 (#2)
  Encode（FastQuantizeEx）~2,800 ms   ← 已优化
  其他                      ~500 ms
  ─────────────────────────────────
  Build complete          21,875 ms
```

两个瓶颈均未进行任何并行化处理，合计占剩余 Phase B 的 **38%**。

### 瓶颈 #1：d_k 校准（4.3 s）— `ConANN::CalibrateDistanceThreshold`

代码位置：`src/index/conann.cpp:102-115`

```cpp
for (uint32_t s = 0; s < num_samples; ++s) {       // 100 次采样
    const float* query = queries + indices[s] * dim;
    for (uint32_t i = 0; i < N; ++i) {              // 1M 次 L2Sqr
        dists[i] = simd::L2Sqr(query, database + i*dim, dim);
    }
    std::nth_element(dists.begin(), ...);            // 串行
    sample_dk[s] = dists[nth];
}
```

- 外层 100 个样本完全独立，**可直接 OMP 并行**
- 内层 L2Sqr 已 SIMD，但外层无并行
- 唯一挑战：`dists` 缓冲区需改为 per-thread 分配（避免数据竞争）

### 瓶颈 #2：CRC 校准（4.2 s）— `CrcCalibrator::ComputeAllScores[RaBitQ]`

代码位置：`src/index/crc_calibrator.cpp:218, 234`

```cpp
for (size_t i = 0; i < query_indices.size(); ++i) {  // ~100-1000 次查询
    all_scores[i] = ComputeScoresForQuery(...);        // 每次完全独立
}
```

- 每次 `ComputeScoresForQuery` 在内部分配所有临时数据（heap、centroid_dists）
- `all_scores[i]` 写入不同索引，**天然线程安全**
- 可直接加 `#pragma omp parallel for schedule(dynamic)` — 零代码重构

## What

1. **d_k OMP 并行化**：在 `CalibrateDistanceThreshold` 中对 100 个样本做 OMP 并行
   - 将共享的 `dists` 缓冲区改为 per-thread 分配
   - 结果 `sample_dk` 按 index 写入，并行安全

2. **CRC OMP 并行化**：在 `ComputeAllScores` 和 `ComputeAllScoresRaBitQ` 中加 OMP pragma
   - 仅需一行 pragma，零额外重构
   - 将 `#include <omp.h>` 加入 `crc_calibrator.cpp`

## Expected Impact

```
优化前（本 change 前）：
  d_k  = 4,260 ms
  CRC  = 4,196 ms
  合计 = 8,456 ms

优化后（8 核 OMP，保守估计 4-6× 加速）：
  d_k  ≈  700-1000 ms
  CRC  ≈  700-1000 ms
  合计 ≈ 1,400-2,000 ms

Deep1M Phase B：21,875 ms → ~15,000-16,000 ms（相比原始 90,500 ms ≈ 5.6-6× 总加速）
```

## Non-Goals

- 不修改 CRC 校准的数学逻辑，只加速计算
- 不改变 d_k 的采样策略或百分位算法
- 不修改查询延迟路径（Phase D）
- 不减少 `num_samples=100`（可作为独立优化）
