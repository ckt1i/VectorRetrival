## ADDED Requirements

### Requirement: Resident FastScan query path SHALL use a query-wrapper-driven prepared view
在 `full_preload + use_resident_clusters=1` 的 resident 查询路径下，系统 SHALL 将 query-global 状态与 per-cluster prepared state 逻辑分离。resident query path MUST 以可复用 query wrapper 持有 `rotated_q` 与 prepare 所需 scratch，并为每个 cluster 生成一个仅包含 FastScan 与 Stage2 所需字段的 prepared view，而不是要求每个 cluster 重新构造独立且长期持有的胖 `PreparedQuery` 对象。

#### Scenario: Once-per-query wrapper is reused across probed clusters
- **WHEN** 一个 resident 查询在同一次 `Search()` 中 probe 多个 cluster
- **THEN** query wrapper MUST 只初始化一次
- **AND** 每个 cluster 的 prepare MUST 复用该 wrapper 中的 scratch 和 query-global 状态

#### Scenario: Prepared view only exposes cluster-local FastScan inputs
- **WHEN** 一个 cluster 完成 prepare
- **THEN** 下游 Stage1 / Stage2 MUST 能从 prepared view 读取 `norm_qc`、`norm_qc_sq`、`sum_q`、`fs_width`、`fs_shift` 和当前 cluster 对应的 FastScan-ready 数据
- **AND** query wrapper 中的 scratch MUST 不需要作为长期公共语义泄露给下游逻辑

### Requirement: Pre-rotated prepare SHALL preserve existing semantic equivalence
resident FastScan query wrapper 使用 pre-rotated 路径时，系统 MUST 继续将 `rotated_q - rotated_centroid` 视为精确的 pre-rotated residual。新的 wrapper / prepared view 组织方式 SHALL 保持与参考路径相同的 Stage1 distance estimate 和 Stage2 estimation 语义。

#### Scenario: Stage1 distance estimates remain equivalent after wrapper refactor
- **WHEN** 相同的 `rotated_q` 与 `rotated_centroid` 进入优化后的 prepare 路径
- **THEN** Stage1 FastScan distance estimates MUST 与参考实现一致

#### Scenario: Stage2 still has access to normalized rotated residual semantics
- **WHEN** 一个 cluster 完成优化后的 pre-rotated prepare
- **THEN** Stage2 所需的 `rotated` 语义、`norm_qc` 和 `norm_qc_sq` MUST 仍然可用
- **AND** ExRaBitQ 路径的结果语义 MUST 保持不变

### Requirement: Prepare substeps SHALL remain observable and ready for follow-up SIMD work
query-wrapper 驱动的 prepare 流程 MUST 保持清晰的子步骤边界，至少能够区分 `subtract_norm`、`normalize_sign_sum_maxabs`、`quantize` 与 `lut_build` 四段。实现 MAY 在后续版本中将这些步骤进一步融合，但本轮重构后这些子步骤 SHALL 仍然是稳定的观测和优化边界。

#### Scenario: Prepare substeps can be reported in analysis mode
- **WHEN** 系统在分析或 benchmark 模式下运行 resident FastScan prepare
- **THEN** 实现 MUST 能分别记录或导出 `subtract_norm`、`normalize_sign_sum_maxabs`、`quantize` 和 `lut_build` 的耗时

#### Scenario: Follow-up SIMD candidates are attached to stable prepare boundaries
- **WHEN** 这次 query-wrapper 重构完成并进入下一轮性能分析
- **THEN** `quantize + LUT build` 融合路径 MUST 已经成为稳定的批量 prepare 边界
- **AND** 后续 SIMD 优化 SHALL 可以围绕这一边界进行，而不需要再次重写整体 query wrapper 生命周期
