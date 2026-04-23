## ADDED Requirements

### Requirement: FastScan Stage1 prepare SHALL keep fine-grained prepare observability
在优化后的 resident FastScan Stage1 路径中，系统 MUST 继续独立观测 `prepare_subtract`、`prepare_normalize`、`prepare_quantize` 和 `prepare_lut_build`。后续实现 MAY 重构 fused prepare 内部结构，但 MUST 不通过合并、移除或黑盒化这些边界来隐藏收益或回归。

#### Scenario: Prepare substeps remain visible after hot-path optimization
- **WHEN** benchmark 或分析模式在优化后的 FastScan Stage1 prepare 路径上运行
- **THEN** 输出 MUST 继续包含 `prepare_subtract`
- **AND** MUST 继续包含 `prepare_normalize`、`prepare_quantize` 和 `prepare_lut_build`

### Requirement: FastScan Stage1 optimization SHALL prioritize estimate kernel over post-estimate control flow
在 resident 主工作点下，若系统继续优化 Stage1 路径，优化边界 MUST 允许将重点放在 `EstimateDistanceFastScan` 主估计循环，而不是重新把 `mask`、`iterate` 和 `classify` 后处理与估计主循环混合为不可区分的一体路径。

#### Scenario: Estimate kernel remains a distinct optimization boundary
- **WHEN** 系统对 FastScan Stage1 进行后续优化
- **THEN** `EstimateDistanceFastScan` 主估计循环 MUST 保持为独立可定位的优化边界
- **AND** `mask`、`iterate` 和 `classify` 后处理 MUST 不重新吞并该边界
