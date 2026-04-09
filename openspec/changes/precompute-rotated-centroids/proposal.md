## Why

当前每次查询中，对每个被探测的 cluster，`PrepareQueryInto` 都会完整执行一次 FWHT（Fast Walsh-Hadamard Transform）来旋转 per-cluster 残差向量：

```
for each probed cluster i (nprobe 次):
    residual = q - c_i                  // O(L)
    norm_qc  = ‖residual‖               // O(L)
    normalize: residual /= norm         // O(L)
    FWHT(residual) → rotated            // O(L log L) ← 每次都重新算
    sign-quantize + sum_q               // O(L)
    BuildFastScanLUT                    // O(L)
```

而根据正交变换的线性性：

```
P^T × (q - c_i) = P^T × q - P^T × c_i
```

如果在构建阶段预先计算并存储所有 centroid 的旋转结果 `rotated_c[i] = P^T × c_i`，查询时只需对 query 做一次 FWHT，然后对每个 cluster 做一次向量减法（O(L)）——彻底消除 per-cluster 的 FWHT 调用。

| 数据集     | dim  | nprobe | 当前 per-cluster FWHT | 优化后 per-cluster |
|-----------|------|--------|----------------------|--------------------|
| COCO 100K | 512  | ~70 (avg_probed) | O(512×9) ≈ 4608 ops | O(512) 向量减法 |
| Deep1M    | 128  | ~86 (avg_probed) | O(128×7) ≈ 896 ops  | O(128) 向量减法 |

**当前基线（performance governor，稳定态）：**
- COCO 100K: avg=1.742ms, p50=1.688ms, p95=2.221ms, p99=2.587ms
- Deep1M:    avg=0.993ms, p50=0.989ms, p95=1.705ms, p99=2.140ms

perf 分析显示 `PrepareQuery` + `RotationMatrix::Apply` 合计约 1.67% 的采样（因 Phase B/C 噪声稀释，实际在 Phase D 中占比更高）。结合之前旧 baseline 的观测（PrepareQueryInto ~10% / 0.4ms），FWHT 是 per-cluster setup 中最贵的部分。

## What Changes

- **构建阶段**：计算并持久化所有 `nlist` 个 centroid 的旋转坐标 `rotated_centroids[i] = P^T × c_i`（MKL `cblas_sgemm` 或批量 FWHT，一次性开销）
- **查询阶段**：新增 `PrepareQueryRotated` 路径，对 query 做一次 FWHT 得到 `rotated_q`，然后每个 cluster 的旋转残差直接由 `rotated_q - rotated_centroids[i]` 计算（O(L) 减法），跳过 per-cluster FWHT
- **范数计算**：利用正交变换保范性 `‖P^T(q-c_i)‖ = ‖q-c_i‖`，可从旋转空间直接计算 `norm_qc²`，无需额外 pass
- **内存开销**：`nlist × dim × sizeof(float)` — COCO: 2048×512×4 = 4 MB；Deep1M: 4096×128×4 = 2 MB
- **CRC 校准路径**同步优化（`ComputeScoresForQueryRaBitQ` 中目前每 cluster 调用 `PrepareQuery`，同样受益）

## Capabilities

### New Capabilities

- `rotated-centroid-cache`: 构建阶段预计算并缓存所有 centroid 的旋转坐标（Hadamard 空间）
- `query-rotate-once`: 查询路径中对 query 只做一次 FWHT，per-cluster 改为 O(L) 向量减法

### Modified Capabilities

- `PrepareQueryInto`: 新增 pre-rotated query 参数路径，跳过 FWHT 步骤
- `ComputeScoresForQueryRaBitQ` (CRC): 同步使用 rotated centroids

## Impact

- **代码路径**：
  - `benchmarks/bench_vector_search.cpp` — 构建时计算 `rotated_centroids`，查询时传入 `rotated_q`
  - `src/rabitq/rabitq_estimator.cpp` — `PrepareQueryInto` 新增重载或模式参数
  - `include/vdb/rabitq/rabitq_estimator.h` — 新增 `PrepareQueryRotatedInto` 接口
  - `src/index/crc_calibrator.cpp` — 同步更新
- **内存**：+4 MB（COCO）/ +2 MB（Deep1M）的旋转 centroid 存储
- **兼容性**：非破坏性变更，原有 `PrepareQueryInto` 接口保持不变；Hadamard 模式专用，非 Hadamard rotation 时退回原路径
- **依赖**：无新外部依赖，利用已有 FWHT 和 MKL 基础设施

## Non-Goals

- 不改变 FastScan LUT 构建逻辑
- 不改变 CRC 校准的统计算法（只优化其内部的旋转开销）
- 不引入多线程并行化
- 不改变 Encoder 路径（Encode 在构建阶段，不影响查询）
