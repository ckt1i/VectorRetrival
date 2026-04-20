## ADDED Requirements

### Requirement: E2E benchmark SHALL support both explanatory timing mode and clean profiling mode
`bench_e2e` MUST 同时支持两类口径：

- 细粒度解释模式：用于 full E2E 报告 prepare / Stage1 / submit 等字段
- 低干扰 profiling 模式：用于 clean perf，减少细粒度计时对 CPU 热点的污染

系统 SHALL 通过明确配置区分这两类模式，并 SHALL 在输出中保留该模式信息。

#### Scenario: Fine-grained timing mode is explicit in benchmark output
- **WHEN** benchmark 以细粒度解释模式运行
- **THEN** 输出 MUST 标明该模式已启用
- **AND** 结果中 MUST 包含 prepare / Stage1 子步骤等细分字段

#### Scenario: Clean profiling mode is explicit in benchmark output
- **WHEN** benchmark 以低干扰 profiling 模式运行
- **THEN** 输出 MUST 标明该模式已启用
- **AND** 结果 MUST 可用于 query-only perf，而不会因细粒度计时造成明显热点污染

### Requirement: This change SHALL be validated by one fixed-parameter full E2E run and one same-parameter query-only perf run
这次 `fastscan-query-wrapper-block-classify-optimization` 变更完成后，benchmark 验收 MUST 包含两轮固定参数测试：

- 一轮 full E2E：报告 recall 与端到端延迟
- 一轮 query-only perf：报告 query 主路径热点

两轮测试 SHALL 使用相同数据集与 query 参数，只允许 benchmark mode 与 timing mode 不同。

#### Scenario: Full E2E validation reports recall and latency
- **WHEN** 变更实现完成并执行 full E2E 验证
- **THEN** 结果 MUST 报告 recall@1、recall@5、recall@10
- **AND** MUST 报告 avg、p50、p95、p99 与 query 主路径细分时间字段

#### Scenario: Query-only clean perf reports hotspot migration
- **WHEN** 变更实现完成并执行同参数 query-only perf
- **THEN** 结果 MUST 能用于判断 `PrepareQueryRotatedInto`、`BuildFastScanLUT`、Stage1 classify 和 batch submit 等热点是否迁移

### Requirement: Benchmark output SHALL explicitly preserve the next SIMD candidate boundaries
在这次 query-wrapper 与 Stage1 block-driven 改造完成后，benchmark 输出或分析结果 MUST 能明确定位后续 SIMD 候选边界，至少包括：

- prepare 中的 `quantize` / `lut_build`
- Stage1 中的 block classify / compaction
- address batch decode
- batch submit 前整理

#### Scenario: Follow-up SIMD candidates are attributable after this change
- **WHEN** clean perf 在新主路径上完成一次分析
- **THEN** 结果 MUST 足以判断哪些批量边界已经稳定
- **AND** 这些边界 MUST 足以支撑下一轮 SIMD 优先级决策
