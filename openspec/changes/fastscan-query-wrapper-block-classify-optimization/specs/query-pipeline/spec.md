## ADDED Requirements

### Requirement: Resident query pipeline SHALL support query-wrapper-driven prepare
在 `clu_read_mode=full_preload` 且 `use_resident_clusters=1` 的 resident 查询路径下，系统 MUST 支持 query-wrapper 驱动的 prepare 流程。一次 `Search()` 中的 query-global 状态 SHALL 只构建一次，并 SHALL 被后续多个 cluster probe 复用。

#### Scenario: Resident query-global state is built once per Search
- **WHEN** resident 模式下同一次 `Search()` probe 多个 cluster
- **THEN** query-global 状态 MUST 只初始化一次
- **AND** 每个 cluster prepare MUST 复用该状态

### Requirement: Query pipeline SHALL support block-driven candidate batches in Stage1
resident 查询路径 MUST 支持 Stage1 block-driven candidate batch。系统 SHALL 允许 `ClusterProber` 将一个 FastScan block 内的 survivor compact 成 candidate batch，并将这个 batch 交给后续地址解码与 sink 提交逻辑，而不是强制维持逐候选主路径。

#### Scenario: Stage1 produces candidate batches instead of only per-candidate callbacks
- **WHEN** 一个 FastScan block 中存在多个非-SafeOut 候选
- **THEN** resident 查询路径 MUST 能生成 candidate batch
- **AND** 下游路径 MUST 能在不改变结果语义的前提下消费这个 batch

#### Scenario: Candidate batch keeps final result semantics
- **WHEN** resident 查询路径改用 block-driven batch 组织
- **THEN** recall、最终排序和候选语义 MUST 与参考逐候选路径保持一致

### Requirement: Query pipeline SHALL expose batch address decode and batch submit as formal optimization boundaries
resident 查询路径 MUST 将 address batch decode 和 batch submit 视为正式边界。实现 MAY 在第一轮保留标量内部实现，但 `ParsedCluster` 与 sink 的接口 SHALL 已经支持批量调用，以便后续继续做 SIMD 或数据布局优化。

#### Scenario: Address batch decode is available on the resident path
- **WHEN** resident 路径处理一个 candidate batch
- **THEN** 系统 MUST 能以批量接口解码其中的地址

#### Scenario: Batch submit is available on the resident path
- **WHEN** resident 路径处理一个 candidate batch
- **THEN** sink MUST 能以批量接口组织后续提交
- **AND** 不得要求逐候选回退到通用提交流程作为唯一实现方式
