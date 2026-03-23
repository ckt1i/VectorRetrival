# Spec: Per-Cluster Epsilon

## Requirements

### R1: Reconstruction Error Computation
- 对每个 cluster，计算（采样）向量的 RaBitQ 重建误差 `L2(v, v̂)`
- 重建公式: `v̂ = centroid + norm · P × (signs/√dim)`
- 采样策略: 若 `cluster_size < 2 × n_samples`，则 `n_samples = max(cluster_size / 2, 1)`；各 cluster 独立计算 n_samples 后取全局最大值

### R2: Per-Cluster Epsilon Storage
- `ClusterLookupEntry` 新增 `float epsilon` 字段
- 文件格式版本 v5 → v6
- Reader 只接受 v6 格式
- epsilon 写入/读取在 Lookup Table 序列化/反序列化中完成

### R3: Per-Cluster ConANN Classification
- `ParsedCluster` 新增 `float epsilon`
- `ProbeCluster` 使用 per-cluster epsilon 和全局 d_k 构造局部 ConANN
- 不再依赖 `index_.conann()` 的全局 epsilon

### R4: Benchmark Output
- `bench_rabitq_accuracy` 输出每个 cluster 的向量数和 epsilon 值
- 格式: 表格，包含 cluster_id, num_vectors, epsilon 三列

## Constraints

### C1: Build-Time Only
- epsilon 标定只在 build 阶段进行（IvfBuilder），不影响查询延迟
- Build 时有 RotationMatrix，可做 ApplyInverse

### C2: Backward Incompatible
- v6 格式不兼容 v5 及更早版本
- Reader 拒绝非 v6 文件

### C3: P95 Percentile
- 使用 P95（而非 P99 或 max）作为 epsilon 百分位
- 平衡 SafeOut 安全性和 Uncertain 比例

## Acceptance Criteria

- [ ] cluster_store_test: epsilon 写入后读回值一致（roundtrip）
- [ ] overlap_scheduler_test: 使用 per-cluster epsilon，全量探测时结果与 brute-force L2 匹配
- [ ] bench_rabitq_accuracy: 运行输出包含 per-cluster statistics 表格
- [ ] 编译无 warning，所有现有 test 通过
