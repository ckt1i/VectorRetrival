## ADDED Requirements

### Requirement: 系统 SHALL 支持 Faiss-style single IVF coarse clustering
系统 SHALL 支持使用 Faiss IVF coarse quantizer 训练 centroids，并使用 nearest-centroid single assignment 为所有 base vectors 生成 primary assignments，作为现有系统可消费的 coarse partition 输入。

#### Scenario: Faiss builder exports single centroids and assignments
- **WHEN** 以 `faiss_kmeans` coarse builder 构建 single partition
- **THEN** 系统 SHALL 导出 centroids 和每个 base vector 的 single primary assignment，且该 assignment SHALL 仅包含一个 coarse cluster id

#### Scenario: Faiss builder shares the same dataset and metric contract
- **WHEN** 在某个数据集、metric 和 `nlist` 上运行 `faiss_kmeans`
- **THEN** 系统 SHALL 使用与现有 single builder 对照相同的数据准备、normalization 与 metric 语义，除非显式元数据另有记录

### Requirement: Faiss parity outputs SHALL be consumable by existing single-mode build and serving flows
系统 SHALL 将 Faiss coarse clustering 的输出适配为现有 single-mode IVF build 与 serving 工作流可直接消费的工件，而不要求同时复用 baseline 的整个搜索链路。

#### Scenario: Existing system consumes Faiss-style partition artifacts
- **WHEN** `faiss_kmeans` 完成 centroids / assignments 导出
- **THEN** 现有 single-mode partition build 流程 SHALL 能在不依赖 overlap 或 secondary assignment 的条件下消费这些工件

#### Scenario: Faiss parity does not require baseline search path replacement
- **WHEN** 使用 `faiss_kmeans` single partition 对照现有系统
- **THEN** serving 对照 SHALL 保持现有 query / candidate / rerank 路径不变，只替换 coarse partition 来源
