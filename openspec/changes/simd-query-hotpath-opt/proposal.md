## Why

当前单线程向量搜索在 CRC 启用场景下平均延迟存在优化空间。通过对两个代表性数据集的查询热路径进行 SIMD 向量化和算法级剪枝来压低延迟：

| 数据集 | dim | N | nlist | nprobe | CRC baseline |
|--------|-----|---|-------|--------|-------------|
| COCO 100K | 512 | 100K | 2048 | 150 | ~2.15 ms |
| Deep1M | 96→128 (pad) | 1M | 4096 | 100 | ~1.35 ms |

查询热路径中，per-cluster setup 开销（PrepareQueryInto ~12%）和 L2Sqr 精确重排（~40%）是两大瓶颈。

**验证策略**: 每个优化点实施后立即在两个数据集上独立验证效果，若出现回退则回滚该优化。

## What Changes

- **PrepareQueryInto SIMD 向量化**: 将 residual 减法、norm 计算、normalize、sign_code 生成、sum_q 累加等 5 个标量循环融合为 2 个 AVX-512 向量化扫描，预期减少 50-70% 的 per-cluster setup 开销。
- **L2Sqr 4x 展开**: 将 AVX-512 L2Sqr 主循环 4x 展开，使用 4 个独立累加器消除 fmadd 的 RAW 依赖链，提升流水线吞吐。
- **Cluster-level 快速跳过**: 在 PrepareQueryInto 之前利用已预计算的 per_cluster_r_max 进行轻量级距离下界判断，跳过所有向量必然 SafeOut 的 cluster。
- **内联 ClassifyAdaptive**: 将 ConANN 分类函数标记为 `VDB_FORCE_INLINE`，消除每个候选向量的函数调用开销。
- 不实施 AccumulateBlock 预取（实测效果不佳）。

## Capabilities

### New Capabilities

- `simd-prepare-query`: PrepareQueryInto 热路径 SIMD 向量化（residual/norm/normalize/sign_code/sum_q 融合扫描）
- `l2sqr-unroll`: L2Sqr AVX-512 4x 展开与独立累加器
- `cluster-guard`: Cluster-level 快速跳过机制（利用 per_cluster_r_max 避免无效 PrepareQueryInto）
- `inline-classify`: ClassifyAdaptive 强制内联

### Modified Capabilities

## Impact

- **代码路径**:
  - `src/rabitq/rabitq_estimator.cpp` — PrepareQueryInto 核心循环重写
  - `src/simd/distance_l2.cpp` — L2Sqr AVX-512 路径 4x 展开
  - `include/vdb/simd/distance_l2.h` — L2Sqr 声明（可能改为 header-only inline）
  - `include/vdb/index/conann.h` — ClassifyAdaptive 添加 `VDB_FORCE_INLINE`
  - `benchmarks/bench_vector_search.cpp` — 添加 cluster guard 逻辑
- **依赖**: 无新外部依赖，仅依赖已有的 AVX-512 intrinsics
- **兼容性**: 无 API 变更，纯性能优化，所有现有测试应继续通过
- **SIMD 函数统一路径**: 所有 SIMD 向量化函数的实现统一放置到 `include/vdb/simd/` 下
- **双数据集验证**: 每个优化点在 COCO 100K 和 Deep1M 上独立测试，回退则丢弃
