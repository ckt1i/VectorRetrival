## MODIFIED Requirements

### R1: Reconstruction Error Computation
- 对每个 cluster，计算（采样）向量的 RaBitQ 重建误差 `L2(v, v̂)`
- 重建公式: `v̂ = centroid + norm · P × (signs/√dim)`
- 采样策略: 若 `cluster_size < 2 × n_samples`，则 `n_samples = max(cluster_size / 2, 1)`；各 cluster 独立计算 n_samples 后取全局最大值
- **新增**: `CalibrateDistanceThreshold` 支持 query/database 分离校准

#### Scenario: d_k 从独立 query 样本校准
- **WHEN** `IvfBuilderConfig.calibration_queries != nullptr` 且 `num_calibration_queries > 0`
- **THEN** `CalibrateDk` 使用 query 样本搜索 database，计算 top-K 第 K 近距离的指定 percentile 作为 d_k

#### Scenario: d_k 从 database 自身校准（fallback）
- **WHEN** `IvfBuilderConfig.calibration_queries == nullptr`
- **THEN** `CalibrateDk` 从 database 向量自身采样 pseudo-query（现有行为不变）

## ADDED Requirements

### Requirement: CalibrateDistanceThreshold query/database 分离重载

`ConANN::CalibrateDistanceThreshold` SHALL 提供新重载，接受独立的 query 和 database 数组。新重载不存在 self-distance 问题（query 不在 database 中）。旧签名 SHALL 保留为 wrapper 调用新重载（queries = database = vectors）。

#### Scenario: 跨模态校准
- **WHEN** 调用新重载传入 text embeddings 作为 queries、image embeddings 作为 database
- **THEN** d_k 反映真实 text→image 距离分布，而非 image→image

#### Scenario: 旧签名兼容
- **WHEN** 调用旧 `CalibrateDistanceThreshold(vectors, N, dim, ...)`
- **THEN** 行为与之前完全一致（从 vectors 自身采样 pseudo-query）

### Requirement: IvfBuilderConfig 可选 query 样本

`IvfBuilderConfig` SHALL 新增 `const float* calibration_queries` 和 `uint32_t num_calibration_queries` 字段，默认为 nullptr/0。`Build()` 签名 SHALL 不变。

#### Scenario: 同模态构建无需改动
- **WHEN** 未设置 `calibration_queries`（默认 nullptr）
- **THEN** 现有 12+ 处 Build() 调用行为不变，d_k 从 database 自采样

#### Scenario: 跨模态构建传入 query
- **WHEN** 设置 `calibration_queries` 指向有效 query 数组
- **THEN** d_k 从 query→database 距离校准

### Requirement: Benchmark d_k 校准修正

`bench_rabitq_accuracy` Phase 3 SHALL 使用 qry 向量（而非 img 向量）校准 d_k。

#### Scenario: COCO text→image benchmark
- **WHEN** 运行 `bench_rabitq_accuracy --dataset coco_1k`
- **THEN** d_k 从 text→image 距离校准，SafeIn/SafeOut 分类结果非退化（不全为 SafeOut）
