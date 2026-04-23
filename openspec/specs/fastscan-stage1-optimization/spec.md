## ADDED Requirements

### Requirement: FastScan Stage1 SHALL minimize online query preparation state
单 bit FastScan 的 query prepare 路径 MUST 将 query 无关或可复用的结构前移，并将在线查询需要长期保留的 `PreparedQuery` 字段压缩到最小必要集合。实现 MAY 使用局部 scratch buffer 作为中间过渡，但这些中间态 MUST 不成为 query 路径长期暴露的重量级状态。

#### Scenario: Temporary FastScan preparation state does not escape query preparation
- **WHEN** 一次 resident query 完成 Stage1 准备
- **THEN** 仅用于构建 LUT 或中间量化的临时 buffer MUST 保持为局部 scratch 状态
- **AND** 它 MUST 不作为下游 probe 路径的长期依赖字段泄露出去

#### Scenario: Stage1 equivalence is preserved after preparation-state slimming
- **WHEN** 优化后的 prepare 路径处理同一个 query 与 centroid
- **THEN** 生成的 Stage1 distance estimate MUST 与参考实现保持等价
- **AND** 后续 Stage2 / rerank 结果语义 MUST 保持不变

### Requirement: FastScan Stage1 SHALL expose and optimize prepare and classification substeps
系统 MUST 把单 bit FastScan 的主路径至少区分为 `probe_prepare_ms`、`probe_stage1_ms` 和 `probe_stage2_ms` 三段，并 SHALL 允许针对 `PrepareQueryRotatedInto`、LUT 构建和 `EstimateDistanceFastScan` 主循环分别实施优化。优化后的实现 MUST 保留这些观测字段，以便 benchmark 和 perf 能归因收益来源。

#### Scenario: Benchmark output keeps Stage1 observability after optimization
- **WHEN** benchmark 在优化后的 resident 路径上运行
- **THEN** 输出 MUST 继续包含 `probe_prepare_ms`
- **AND** 输出 MUST 继续包含 `probe_stage1_ms`
- **AND** 输出 MUST 继续包含 `probe_stage2_ms`

#### Scenario: Query path optimization does not collapse Stage1 into an opaque total
- **WHEN** 后续实现压缩 `PrepareQueryRotatedInto`、LUT 构建或 `EstimateDistanceFastScan`
- **THEN** 这些优化 MUST 仍然能够被映射回现有 Stage1 相关时间字段
- **AND** 系统 MUST 不以重新合并统计字段的方式隐藏收益或回归

## ADDED Requirements (from fastscan-prepare-hotpath-optimization)

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
