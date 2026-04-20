## MODIFIED Requirements

### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL support direct before/after comparison between the existing sliding-window `.clu` loading path and the new full-preload `.clu` loading path under the same warm-serving query settings. 对 resident 主路径优化相关实验，benchmark 还 MUST 能在固定参数下输出同口径的 full E2E 结果与 query-only perf 结果，以支持主路径收益归因。

#### Scenario: Same operating point is run under both loading modes
- **WHEN** the benchmark evaluates the `.clu` preload optimization
- **THEN** it SHALL run the same dataset and search parameters under both `window` and `full_preload` modes
- **AND** the exported results SHALL make the loading mode explicit

#### Scenario: Query-speed comparison fields are exported
- **WHEN** a loading-mode comparison result is exported
- **THEN** the result SHALL include `recall@10`, `e2e_ms`, and `p99_ms`
- **AND** it SHALL include enough metadata to reconstruct the exact operating point being compared

#### Scenario: Preload cost is recorded alongside query results
- **WHEN** `full_preload` mode is benchmarked
- **THEN** the benchmark output SHALL record the preload time and preload-related resident memory or byte footprint
- **AND** these fields SHALL be reported alongside the query-speed result rather than omitted

#### Scenario: Resident hotpath optimization exports full E2E and query-only evidence
- **WHEN** the benchmark evaluates this resident query hotpath / FastScan optimization under the fixed resident parameter set
- **THEN** it MUST produce one full E2E result with recall and end-to-end latency
- **AND** it MUST produce one query-only profiling run under the same query parameters for perf analysis

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证和 resident 主路径分析的 benchmark 输出 MUST 包含分析候选流与 query 主路径变化所需的统计，而不能只输出最终延迟。除现有 Stage 1 / Stage 2 候选计数外，resident 主路径运行还 MUST 导出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms` 和 `probe_submit_ms`。

#### Scenario: 候选流统计被导出
- **WHEN** 一次 epsilon 验证运行结束时
- **THEN** 输出中 MUST 包含 `avg_total_probed`
- **AND** MUST 包含 Stage 1 `SafeOut`
- **AND** MUST 包含 Stage 2 `SafeOut`
- **AND** MUST 包含 Stage 2 `Uncertain`
- **AND** MUST 包含 `avg_probe_time_ms`

#### Scenario: Resident query-path breakdown is exported
- **WHEN** benchmark 在 resident 优化路径上运行
- **THEN** 输出中 MUST 包含 `avg_coarse_select_ms`
- **AND** MUST 包含 `avg_probe_prepare_ms`
- **AND** MUST 包含 `avg_probe_stage1_ms`
- **AND** MUST 包含 `avg_probe_stage2_ms`
- **AND** MUST 包含 `avg_probe_classify_ms`
- **AND** MUST 包含 `avg_probe_submit_ms`
