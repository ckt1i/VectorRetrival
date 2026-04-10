## Context

当前向量搜索热路径在 CRC 启用场景（avg_probed=71, SafeOut=97.2%）下的时间分布：

- **L2Sqr 精确重排**: ~40%（内存延迟瓶颈，32 cache lines/vector，512-dim）
- **PrepareQueryInto**: ~12%（5 个标量循环 × 512 迭代，per-cluster 调用 71 次）
- **FastScan AccumulateBlock + Dequantize**: ~8%
- **BuildFastScanLUT**: ~7%
- **Rotation (FWHT)**: ~8%（已 AVX-512 优化）
- **IPExRaBitQ**: ~7%
- **MKL sgemv (centroid dist)**: ~1%

关键约束：dim=512，AVX-512 (Ice Lake-SP)，LTO 启用，CRC 场景下 per-cluster setup 占比显著提升。

## Goals / Non-Goals

**Goals:**
- PrepareQueryInto 的标量循环全部 AVX-512 向量化并融合
- L2Sqr AVX-512 路径 4x 展开，消除 fmadd RAW 依赖
- 在 cluster 粒度实现快速跳过，避免无效 PrepareQueryInto
- ClassifyAdaptive 强制内联，消除函数调用开销
- 所有 SIMD 函数实现统一放到 `include/vdb/simd/` 下

**Non-Goals:**
- AccumulateBlock 预取（实测效果不佳）
- FastScan LUT 构建向量化（复杂度高、收益低）
- 多线程并行化
- 改变 CRC 校准算法或参数

## Decisions

### Decision 1: PrepareQueryInto — 两遍融合 SIMD 扫描

**选择**: 将 5 个标量循环融合为 2 个 AVX-512 扫描
- Scan 1: `residual = q - c` 同时累加 `norm_sq = Σ residual²`
- Scan 2: `normalize * inv_norm` 同时生成 `sign_code` + 累加 `sum_q`

**替代方案**: 单遍扫描全部 5 个操作 — 不可行，normalize 依赖 norm_sq 结果

**SIMD 函数放置**: 新增 `include/vdb/simd/prepare_query.h`（header-only inline），提供：
- `SimdSubtractAndNormSq()` — AVX-512 融合减法+平方和
- `SimdNormalizeSignSum()` — AVX-512 融合 normalize+sign_code+sum_q

在 `rabitq_estimator.cpp` 的 `PrepareQueryInto` 中调用这些 SIMD 函数替代标量循环。

### Decision 2: L2Sqr 4x 展开 — 4 个独立累加器

**选择**: 4 个独立 `__m512 sum0..sum3` 寄存器，每次迭代处理 64 floats（dim=512 → 8 次迭代）

**原理**: `_mm512_fmadd_ps` 延迟 4 cycles，吞吐 1/cycle。单累加器时下一个 fmadd 必须等 4 cycles（RAW dependency）。4 个独立累加器后可以每 cycle 发射 1 条 fmadd。

**放置**: 将 L2Sqr AVX-512 实现移至 `include/vdb/simd/distance_l2.h`（header-only inline），`distance_l2.cpp` 仅保留非 AVX-512 回退。

### Decision 3: Cluster-level guard — r_max 距离下界

**选择**: 在 PrepareQueryInto 之前，利用三角不等式计算 cluster 内所有向量到 query 的距离下界，若下界已超过当前 est_kth 则跳过整个 cluster。

**三角不等式推导**:

对 cluster k 的中心 c_k 和任意成员向量 o：
```
||q - o||₂ ≥ | ||q - c_k||₂ - ||c_k - o||₂ |
           ≥ ||q - c_k||₂ - ||c_k - o||₂      (当 ||q - c_k|| ≥ ||c_k - o|| 时)
           ≥ centroid_dist - r_max_k
```

其中：
- `centroid_dist = ||q - c_k||₂`，即 `centroid_dists[p].first`（已计算并排序）
- `r_max_k = max(||o - c_k||₂)` 对 cluster k 内所有向量，即 `per_cluster_r_max[cid]`（构建阶段预计算）
- 距离下界 `dist_lb = centroid_dist - r_max_k`

**跳过条件**: `dist_lb > est_kth_now`，即：
```cpp
if (centroid_dists[p].first - per_cluster_r_max[cid] > est_kth_now) {
    clusters_skipped++;
    continue;
}
```

**安全性论证**:
- 三角不等式保证 `dist_lb` 是所有 `||q-o||` 的严格下界（不会误跳）
- `est_kth_now` 是当前 est_heap 中第 k 大距离（可能为 infinity，此时条件不满足，不会误跳）
- 无需 `norm_qc` 或 `margin`，因为这是在 RaBitQ 近似之前的几何下界
- 唯一风险：如果 `centroid_dist < r_max`，下界为负，条件自动不满足

**替代方案**: 预计算每个 cluster 的 tightest 包围球半径 — 与当前 r_max 等价，无需额外存储。

### Decision 4: ClassifyAdaptive — header-only 内联

**选择**: 将 `ConANN::ClassifyAdaptive` 实现从 `conann.cpp` 移到 `conann.h`，标记 `VDB_FORCE_INLINE`。

**理由**: 函数体仅 6 条条件判断，热路径中每个非 SafeOut 向量调用一次。内联后编译器可消除调用开销并将分类逻辑融入 ctz 循环。

## Risks / Trade-offs

- **[风险] L2Sqr 展开增加代码体积** → 缓解: dim=512 时仅 8 次迭代，展开后代码仍适合 icache
- **[风险] Cluster guard 误跳过有用 cluster** → 缓解: 使用保守的距离下界估计（不等式方向确保不会误跳），配合 recall 测试验证
- **[风险] SIMD 融合引入数值精度差异** → 缓解: 与标量实现对比误差 < 1e-6，不影响 recall
- **[权衡] header-only 增加 .h 编译时间** → 可接受，LTO 场景下编译时间已较长
- **[风险] 优化在两个数据集上效果不一致** → 缓解: 每个优化点独立验证，COCO(dim=512) 和 Deep1M(dim=96→128) 的向量维度差异可能导致不同表现

## Benchmark 命令参考

**COCO 100K** (dim=512, nlist=2048, nprobe=150):
```bash
./bench_vector_search \
  --base /home/zcq/VDB/data/coco/coco_base.fvecs \
  --query /home/zcq/VDB/data/coco/coco_query.fvecs \
  --gt /home/zcq/VDB/data/coco/coco_groundtruth.ivecs \
  --centroids /home/zcq/VDB/data/coco/coco_centroid_2048.fvecs \
  --assignments /home/zcq/VDB/data/coco/coco_cluster_id_2048.ivecs \
  --nlist 2048 --nprobe 150 --topk 10 --bits 4 --queries 200 --crc 1
```

**Deep1M** (dim=96→128 pad, nlist=4096, nprobe=100, baseline ~1.35ms):
```bash
./bench_vector_search \
  --base /home/zcq/VDB/data/deep1m/deep1m_base.fvecs \
  --query /home/zcq/VDB/data/deep1m/deep1m_query.fvecs \
  --gt /home/zcq/VDB/data/deep1m/deep1m_groundtruth.ivecs \
  --centroids /home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --nlist 4096 --nprobe 100 --topk 10 --bits 4 --queries 200 --crc 1 \
  --pad-to-pow2 1
```
