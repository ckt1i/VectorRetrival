## ADDED Requirements

### Requirement: 粗聚类构建器身份必须可配置并被记录
系统 SHALL 支持使用显式 coarse builder identity 构建兼容的 IVF coarse partition，并 SHALL 在构建元数据和诊断输出中记录所选 builder。

#### Scenario: 使用仓库默认 coarse builder 构建
- **WHEN** 使用仓库默认 coarse builder 构建 coarse partition
- **THEN** 构建输出 SHALL 将 builder identity 记录为 `hierarchical_superkmeans` 或其他显式配置值

#### Scenario: 使用 Faiss coarse builder 构建
- **WHEN** 使用基于 Faiss 的 builder 构建 coarse partition
- **THEN** 构建输出 SHALL 将 builder identity 记录为 `faiss_kmeans`

### Requirement: Builder 对照必须产出可兼容的评估产物
系统 SHALL 为不同 coarse builder 产出能够被同一 pure coarse-cover 诊断工作流消费的 coarse partition 输出，而不要求 builder-specific 的评估逻辑。

#### Scenario: 不同 builder 共享评估契约
- **WHEN** 在同一数据集、metric 和 `nlist` 下使用不同 builder 构建 coarse partition
- **THEN** 诊断工作流 SHALL 在同一 query set、ground truth 和 probe schedule 下评估它们

#### Scenario: 输出中保留 builder-specific 元数据
- **WHEN** 为多个 builder 导出诊断结果
- **THEN** 每一行输出 SHALL 包含用于对照 builder identity 的字段，而不依赖外部手工记录
