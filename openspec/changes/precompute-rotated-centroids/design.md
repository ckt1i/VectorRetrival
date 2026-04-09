## Context

当前查询热路径中 `PrepareQueryInto` 对每个被探测 cluster 均执行一次完整 FWHT：

```
per-query 开销（nprobe ≈ 70~86 次 per-cluster 调用）：
  FWHT (O(L log L)):  dim=512 → ~4608 ops × 70 = ~322K ops
                      dim=128 → ~896 ops  × 86 = ~77K ops
  subtract+norm+normalize: O(L) × nprobe（小）
  sign-quantize+LUT:       O(L) × nprobe（小）
```

数学约束：Hadamard 旋转满足线性性，且保持 L2 范数：
```
P^T × (q - c_i) = P^T×q - P^T×c_i   ← 分解为两项
‖P^T × v‖ = ‖v‖                      ← 正交变换保范
```

当前 performance governor 下稳定态基线：
- COCO 100K: avg=1.742ms, p50=1.688ms（nprobe max=150, avg_probed≈70）
- Deep1M:    avg=0.993ms, p50=0.989ms（nprobe max=100, avg_probed≈86）

## Goals / Non-Goals

**Goals:**
- 消除查询路径中 per-cluster 的 FWHT 调用，改为一次 query FWHT + per-cluster O(L) 减法
- 在构建阶段批量计算并缓存所有 centroid 的旋转坐标
- 保持 recall 不变（变换等价，无精度损失）
- 同步优化 CRC 校准路径中的冗余 FWHT

**Non-Goals:**
- 非 Hadamard rotation（GenerateRandom 路径）不优化（矩阵-向量乘法，无批量优化空间）
- 不改变旋转矩阵的生成/存储方式
- 不修改 FastScan LUT 或 RaBitQ 编码逻辑
- 不做多线程并行化

## Decisions

### Decision 1: 数学推导与变换等价性

原始流程（per cluster i）：
```
residual_i     = q - c_i
norm_qc_i      = ‖residual_i‖
q̄_i           = residual_i / norm_qc_i
rotated_i      = P^T × q̄_i                   // FWHT，O(L log L)
```

优化流程：
```
// [构建阶段，一次性]
rotated_c[i]   = P^T × c_i   ∀i ∈ [0, nlist)  // 批量 FWHT 或 sgemm

// [查询阶段，只做一次]
rotated_q_raw  = P^T × q                        // FWHT，O(L log L)，只算一次！

// [per cluster i，O(L)]
diff_i         = rotated_q_raw - rotated_c[i]   // O(L) 向量减法
norm_qc_i      = ‖diff_i‖                       // = ‖q - c_i‖（保范性）
q̄_i           = diff_i / norm_qc_i              // O(L) 归一化
// 后续：sign-quantize, sum_q, LUT 不变
```

**关键等价性验证：**
```
diff_i = P^T×q - P^T×c_i = P^T×(q - c_i)  ← 线性性
‖diff_i‖ = ‖P^T×(q-c_i)‖ = ‖q - c_i‖      ← 保范性
q̄_i = diff_i / ‖diff_i‖ = P^T×(q̄_orig)   ← 单位向量一致
```

所有后续计算（sign_code, sum_q, FastScan LUT）直接使用 `diff_i/norm` 作为 `rotated`，与原流程完全等价。

### Decision 2: 构建阶段批量计算 rotated_centroids

方案 A（批量 FWHT，推荐）：
```cpp
// 对每个 centroid 逐一调用 rotation.Apply(c_i, rotated_c[i])
// rotation 已有 use_fast_hadamard_ 路径，O(L log L) per centroid
// nlist 次 Apply = nlist × O(L log L)
// Deep1M: 4096 × 896 ops ≈ 3.7M ops（< 1ms）
// COCO:   2048 × 4608 ops ≈ 9.4M ops（< 5ms）
```

方案 B（MKL sgemm）：
```cpp
// rotated_centroids = P^T × Centroids  (dim×nlist = dim×dim · dim×nlist)
// P^T: dim×dim，Centroids: dim×nlist → O(dim² × nlist)
// Deep1M: 128² × 4096 ≈ 67M ops → 较慢
// COCO:   512² × 2048 ≈ 537M ops → 更慢
```

**选择方案 A（批量调用 Apply）**：Hadamard 路径是 O(L log L)，远快于 O(L²) matmul；且 `rotation.Apply` 已有正确的 FWHT + 对角缩放实现，无需重复。

构建时序：在 Phase B 加载 centroid 后、打包 FastScan 块之前，批量计算 `rotated_centroids`：
```cpp
// 在 bench_vector_search.cpp Phase B 中
std::vector<float> rotated_centroids(static_cast<size_t>(nlist) * dim, 0.0f);
for (uint32_t k = 0; k < nlist; ++k) {
    rotation.Apply(
        centroids.data() + static_cast<size_t>(k) * dim,
        rotated_centroids.data() + static_cast<size_t>(k) * dim);
}
```

### Decision 3: 查询路径 API 设计

方案 A：新增 `PrepareQueryRotatedInto` 重载，接受 `const float* rotated_q` 和 `const float* rotated_c`：
```cpp
void PrepareQueryRotatedInto(
    const float* rotated_q,       // 已旋转的 query（单位向量化之前）
    const float* rotated_centroid, // 已旋转的 centroid
    PreparedQuery* pq) const;
```

方案 B：在 `PrepareQueryInto` 加 bool 参数 `use_prerotated`，条件跳过 FWHT。

**选择方案 A**：接口语义更清晰，避免 bool 参数的认知负担；新重载无需改动现有调用点。

查询时流程（`bench_vector_search.cpp` Phase D）：
```cpp
// 一次性计算 rotated_q（在 per-query 开始处）
std::vector<float> rotated_q(dim);
rotation.Apply(q_vec, rotated_q.data());   // 只算一次！

// 内层 cluster 循环：
for (uint32_t p = 0; p < nprobe; ++p) {
    uint32_t cid = centroid_dists[p].second;
    const float* rotated_c = rotated_centroids.data() +
                             static_cast<size_t>(cid) * dim;
    estimator.PrepareQueryRotatedInto(rotated_q.data(), rotated_c, &pq);
    // ... 后续 FastScan/popcount 不变
}
```

### Decision 4: PrepareQueryRotatedInto 实现

关键差异点（相对于 `PrepareQueryInto`）：

| 步骤 | 原 PrepareQueryInto | PrepareQueryRotatedInto |
|------|---------------------|-------------------------|
| 步骤1：subtract | `residual = q - c_i` (标量/SIMD) | `diff = rotated_q - rotated_c_i` (SIMD O(L)) |
| 步骤2：norm_sq | `norm_sq = Σ residual²` | `norm_sq = Σ diff²` (同，可从 rotated 计算) |
| 步骤3：normalize | `residual /= norm` | `diff /= norm` |
| **步骤4：FWHT** | **`Apply(residual, rotated)`** | **跳过！diff 已在旋转空间** |
| 步骤5：sign+sum | `SimdNormalizeSignSum(rotated,...)` | `SimdNormalizeSignSum(diff/norm,...)` |
| 步骤6：LUT | 不变 | 不变 |

实现上可直接复用 `SimdSubtractAndNormSq` 和 `SimdNormalizeSignSum`（已有 AVX-512 实现），只是跳过中间的 `rotation.Apply` 调用。

### Decision 5: CRC 校准路径（crc_calibrator.cpp）

`ComputeScoresForQueryRaBitQ` 中同样有 per-cluster `PrepareQuery` 调用，且**更贵**（探测所有 nlist 个 clusters，不只是 nprobe 个）。

同步优化方案：
1. 函数签名增加 `const float* rotated_centroids` 参数
2. 在函数开头计算 `rotated_q = P^T × q`（一次）
3. 内层循环改用 `PrepareQueryRotatedInto`

预期收益更显著（nlist 次探测，而非 nprobe 次）。

## Risks / Trade-offs

- **[数值精度]** `P^T×q - P^T×c_i` 与 `P^T×(q-c_i)` 在浮点计算中存在微小差异（加减顺序不同），但幅度 < 1e-6，不影响 sign-quantize 的正确性。建议在测试中对比两路径的 `rotated` 输出差异 < 1e-5。
- **[内存]** +4MB (COCO) / +2MB (Deep1M) 的 rotated_centroids 存储。相比当前 SOA code layout（103MB / 264MB），可忽略。
- **[Hadamard 专用]** 该优化仅在 `use_fast_hadamard_=true` 时有效；非 Hadamard rotation 仍走原路径，需要条件分支或两套 API。
- **[未知收益幅度]** perf 数据因 Phase B/C 噪声稀释，Phase D 中 FWHT 的实际占比尚未精确测量。建议实施前先用 Phase-D-only profiling（插桩计时）获取精确数字。实际 FWHT 占 PrepareQueryInto 约 30-50%，PrepareQueryInto 约占查询延迟的 5-10%，预期整体提升 **2-5%**。
- **[CRC 校准收益]** CRC 阶段探测所有 nlist clusters，FWHT 调用 nlist 次（4096次），优化效果更显著。预计 CRC 校准时间减少 20-30%。

## Benchmark 命令参考

**COCO 100K** (dim=512, nlist=2048, nprobe=150):
```bash
./build/benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs \
  --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs \
  --centroids /home/zcq/VDB/data/coco_100k/coco100k_centroid_2048.fvecs \
  --assignments /home/zcq/VDB/data/coco_100k/coco100k_cluster_id_2048.ivecs \
  --nlist 2048 --nprobe 150 --topk 10 --bits 4 --queries 200 --crc 1
```

**Deep1M** (dim=96→128 pad, nlist=4096, nprobe=100):
```bash
./build/benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/deep1m/deep1m_base.fvecs \
  --query /home/zcq/VDB/data/deep1m/deep1m_query.fvecs \
  --gt /home/zcq/VDB/data/deep1m/deep1m_groundtruth.ivecs \
  --centroids /home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --nlist 4096 --nprobe 100 --topk 10 --bits 4 --queries 200 --crc 1 \
  --pad-to-pow2 1
```

**建议：实施前先做精确 Phase D profiling**
```bash
# 在 bench_vector_search.cpp Phase D 开始/结束处添加 PAPI 或 clock_gettime 计时
# 分别计算：FWHT 时间 / PrepareQueryInto 总时间 / 整体查询时间
# 确认 FWHT 在 Phase D 中的实际比例
```
