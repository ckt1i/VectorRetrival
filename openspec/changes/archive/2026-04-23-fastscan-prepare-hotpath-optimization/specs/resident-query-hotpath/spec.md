## ADDED Requirements

### Requirement: Resident query hot path SHALL reuse fixed-capacity prepare scratch in steady state
在 `full_preload + use_resident_clusters=1` 的 resident 查询 hot path 中，prepare 相关的 `PreparedQuery` 与 scratch 缓冲 MUST 支持 steady-state 固定容量复用。查询执行期间，系统 MUST 允许只覆盖本次 prepare 内容，而不是在每个 cluster 上重复执行可前移的 `resize`、对齐重算或同类容器状态维护。

#### Scenario: Resident prepare scratch reuses capacity across probed clusters
- **WHEN** 同一 resident query 连续 prepare 多个 cluster
- **THEN** `PreparedQuery` 与 scratch 缓冲 MUST 复用既有容量和布局
- **AND** 主路径 MUST 不要求在每个 cluster 上重新建立等价的容器状态

### Requirement: Resident query hot path SHALL avoid redundant prepare-buffer clearing when full overwrite is available
在 resident hot path 下，若 prepare 阶段的某个输出缓冲可以由后续计算完整覆盖写入，系统 SHALL 允许省去该缓冲在热路径中的冗余预清理。若存在尾部或部分覆盖边界，系统 MUST 仅对必要部分保留显式清理或 masking。

#### Scenario: Full-overwrite prepare buffer skips redundant clear
- **WHEN** resident prepare 路径生成一个能够被完整覆盖写的输出缓冲
- **THEN** 系统 MAY 跳过该缓冲在热路径中的整段预清理
- **AND** 最终输出语义 MUST 与参考路径保持一致
