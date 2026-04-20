# E2E Benchmark Spec

## Overview

End-to-end benchmark infrastructure for vector search, supporting both sliding-window and full-preload cluster loading modes.

## Requirements

### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL support direct before/after comparison between the existing sliding-window `.clu` loading path and the new full-preload `.clu` loading path under the same warm-serving query settings. 对 resident 主路径优化相关实验，benchmark 还 MUST 能在固定参数下输出同口径的 full E2E 结果与 query-only perf 结果，以支持主路径收益归因.

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

## CLI Parameters

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--dataset` | string | `/home/zcq/VDB/data/coco_1k` | 数据集路径 |
| `--queries` | int | 0 | query 数量限制 |
| `--output` | string | `/home/zcq/VDB/test/` | 输出基础目录 |
| `--nlist` | int | 32 | K-Means 聚类数 |
| `--nprobe` | int | 32 | 搜索探查聚类数 |
| `--topk` | int | 10 | Top-K |
| `--early-stop` | int | 1 | 启用 early stop (1=on, 0=off) |
| `--bits` | int | 1 | RaBitQ 量化位数 |
| `--block-size` | int | 64 | RaBitQ 块大小 |
| `--c-factor` | float | 5.75 | RaBitQ c 因子 |
| `--max-iter` | int | 20 | K-Means 最大迭代 |
| `--seed` | int | 42 | 随机种子 |
| `--page-size` | int | 4096 | 存储页大小 |
| `--p-for-dk` | int | 99 | d_k 校准百分位 |

## Behavior Constraints

- 所有参数的默认值必须与当前硬编码值一致，确保无参数调用时行为不变
- 新增 `GetFloatArg` 辅助函数支持 float 类型参数
- config.json 输出中包含所有参数值（含默认值）

## Additional Requirements (from fastscan-epsilon-bound-validation)

### Requirement: E2E benchmark 必须报告 epsilon 验证元数据
warm-serving benchmark 的输出必须包含足够的元数据，用来区分构建阶段的 epsilon 输入和运行时加载到的 FastScan 边界。

#### Scenario: 输出中可见加载后的 epsilon
- **当** `bench_e2e` 在重建索引或预构建索引上运行时
- **则** 输出中必须包含加载后的运行时 `eps_ip`
- **并且** 输出中必须标明这次运行使用的是重建后的索引，还是通过 `--index-dir` 复用的索引

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证和 resident 主路径分析的 benchmark 输出 MUST 包含分析候选流与 query 主路径变化所需的统计，而不能只输出最终延迟。除现有 Stage 1 / Stage 2 候选计数外，resident 主路径运行还 MUST 导出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms` 和 `probe_submit_ms`。

#### Scenario: 候选流统计被导出
- **当** 一次 epsilon 验证运行结束时
- **则** 输出中必须包含 `avg_total_probed`
- **并且** 输出中必须包含 Stage 1 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `Uncertain`
- **并且** 输出中必须包含 `avg_probe_time_ms`

#### Scenario: Resident query-path breakdown is exported
- **WHEN** benchmark 在 resident 优化路径上运行
- **THEN** 输出中 MUST 包含 `avg_coarse_select_ms`
- **AND** MUST 包含 `avg_probe_prepare_ms`
- **AND** MUST 包含 `avg_probe_stage1_ms`
- **AND** MUST 包含 `avg_probe_stage2_ms`
- **AND** MUST 包含 `avg_probe_classify_ms`
- **AND** MUST 包含 `avg_probe_submit_ms`

## Additional Requirements (from warm-serving-experiment-plan)

### Requirement: E2E benchmark output SHALL support warm Pareto analysis
The E2E benchmark workflow SHALL support warm steady-state reporting as the primary analysis mode and SHALL produce output that is sufficient for recall-latency Pareto comparison and mechanism attribution.

#### Scenario: Warm operating-point sweep is reported
- **WHEN** the benchmark is run for the main post-baseline experiment phase
- **THEN** it SHALL report multiple BoundFetch operating points under the same warm protocol
- **AND** each point SHALL include `recall@10` and `e2e_ms`

#### Scenario: BoundFetch aggregate output captures multi-parameter sweep identity
- **WHEN** a BoundFetch operating point is added to the aggregate output
- **THEN** its parameter field SHALL be rich enough to distinguish `nprobe`, `crc-alpha`, `epsilon-percentile`, and `nlist`
- **AND** the encoding SHALL be stable enough to reconstruct the Pareto source of each point

#### Scenario: Baseline aggregate output captures baseline sweep identity
- **WHEN** a baseline operating point is added to the aggregate output
- **THEN** its parameter field SHALL preserve the baseline-side control parameter used for the sweep
- **AND** that field SHALL be sufficient to reconstruct one Pareto curve per baseline family

#### Scenario: Mechanism fields are preserved for attribution
- **WHEN** a BoundFetch warm result is exported
- **THEN** the benchmark output SHALL preserve mechanism-level fields needed for attribution
- **AND** those fields SHALL include the effective submission and probe breakdown needed to explain the result

#### Scenario: Output location is stable for plan execution
- **WHEN** the main warm benchmark workflow is run
- **THEN** its aggregate result SHALL be recorded in `baselines/results/e2e_comparison_warm.csv`
- **AND** the corresponding narrative interpretation SHALL be updated in `baselines/results/analysis.md`

#### Scenario: Appendix validation is separated from the main result
- **WHEN** queue-depth, submission mode, or SQPOLL checks are run
- **THEN** they SHALL be treated as supporting validation rather than the main benchmark objective
- **AND** the main result SHALL remain the warm recall-latency Pareto comparison

#### Scenario: Reporting fields are complete enough for decision gates
- **WHEN** a warm operating point is added to the aggregate output
- **THEN** it SHALL include the fields required to judge recall-latency tradeoff and mechanism cost
- **AND** those fields SHALL be sufficient to decide whether to continue or stop further optimization

## Additional Requirements (from rair-secondary-assignment-optimization)

### Requirement: E2E benchmark SHALL compare single, naive top-2, and RAIR top-2 under one protocol
The end-to-end benchmark workflow SHALL support direct comparison among `single`, `redundant_top2_naive`, and `redundant_top2_rair` under the same dataset, query count, `nlist`, `nprobe`, `epsilon`, and warm-serving mode.

#### Scenario: Three assignment modes are exported under the same protocol
- **WHEN** the benchmark evaluates the RAIR change
- **THEN** it SHALL be able to run and export results for `single`, `redundant_top2_naive`, and `redundant_top2_rair`
- **AND** the exported results SHALL make the assignment mode explicit

#### Scenario: RAIR parameters are exported with benchmark results
- **WHEN** a `redundant_top2_rair` operating point is exported
- **THEN** the exported record SHALL include the RAIR configuration needed to reproduce the run
- **AND** that configuration SHALL include at least `lambda`

### Requirement: Benchmark evaluation SHALL answer whether RAIR reduces high-recall probing demand
The benchmark plan SHALL not rely only on a fixed-`nprobe` latency comparison. It SHALL include a comparison that can determine whether RAIR reduces the probing demand required to reach the target high-recall region.

#### Scenario: High-recall comparison reports whether target recall is reached at lower probing demand
- **WHEN** `redundant_top2_rair` is compared against `redundant_top2_naive`
- **THEN** the benchmark results SHALL make it possible to determine whether the target recall region is reached with fewer probed clusters or a lower equivalent probing workload
- **AND** the conclusion SHALL be based on measured outputs rather than offline reasoning alone

#### Scenario: Fixed-nprobe comparison remains available
- **WHEN** the benchmark evaluates RAIR at a fixed `nprobe`
- **THEN** it SHALL still export recall, average latency, and tail latency for fair end-to-end comparison
- **AND** those results SHALL be attributable to the assignment mode used to build the index

### Requirement: Benchmark output SHALL expose the cost of RAIR-based redundant serving
The benchmark output SHALL include the fields needed to judge the trade-off of RAIR beyond recall alone, including probe expansion, duplicate-candidate behavior, preload cost, and build cost.

#### Scenario: Duplicate and deduplicated candidate cost is reported
- **WHEN** a redundant-assignment operating point is exported
- **THEN** the result SHALL include a measure before duplicate collapse and a measure after duplicate collapse
- **AND** those fields SHALL remain comparable between naive top-2 and RAIR top-2

#### Scenario: Build and preload overhead is reported
- **WHEN** a RAIR-built index is benchmarked
- **THEN** the exported result SHALL include build-time and preload-cost fields
- **AND** those fields SHALL be directly comparable with `single` and `redundant_top2_naive`

## ADDED Requirements (from resident-thinpath-batch-rerank-optimization)

### Requirement: 在线 E2E benchmark 不得执行 CRC calibration
`bench_e2e` 的在线 benchmark 运行 MUST 不将 CRC calibration 作为执行流程的一部分。在线 benchmark SHALL 直接加载预先生成的 CRC 参数或等价工件；若工件缺失，系统必须以明确错误或明确的非在线口径退出，而不是在同一轮在线 benchmark 中重新 calibration。

#### Scenario: CRC 工件存在时直接加载
- **WHEN** benchmark 运行且索引目录中存在 CRC 参数或等价工件
- **THEN** 系统必须直接加载这些工件并进入在线查询阶段

#### Scenario: CRC 工件缺失时不回退到在线 calibration
- **WHEN** benchmark 运行且缺少所需 CRC 工件
- **THEN** 系统不得在该次在线 benchmark 中执行 CRC calibration 作为隐式回退路径

### Requirement: 分阶段优化必须产出固定口径测速与 perf 结果
本次 resident thin path 与 batched rerank 优化实现 MUST 在第二阶段和第三阶段分别使用相同参数执行 benchmark 测速，并对各阶段结果执行 perf 采样，作为后续优化参考。

#### Scenario: 第二阶段输出测速与 perf
- **WHEN** 第二阶段 resident thin path 实现完成
- **THEN** 系统必须使用约定 benchmark 参数输出速度结果，并产出对应 perf 热点结果

#### Scenario: 第三阶段输出测速与 perf
- **WHEN** 第三阶段 batched rerank 实现完成
- **THEN** 系统必须使用与第二阶段一致的 benchmark 参数再次输出速度结果，并产出对应 perf 热点结果

### Requirement: E2E benchmark 必须输出新分段统计字段
在线 E2E benchmark MUST 输出至少以下字段：`probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: benchmark 输出新统计字段
- **WHEN** benchmark 完成一次查询轮次并写出结果
- **THEN** 结果中必须包含约定的新分段时间和候选计数字段

#### Scenario: benchmark 区分两类原始向量读取口径
- **WHEN** benchmark 输出 `prefetch_*` 与 `pool_vector_read_ms` 等字段
- **THEN** 结果必须能够区分 probe 阶段按 cluster 从磁盘预读原始向量到内存池的开销与 all-probe-done 后从内存池批量读取原始向量用于 rerank 的开销

## ADDED Requirements (from coarse-centroid-score-simd-optimization)

### Requirement: E2E benchmark SHALL preserve coarse score breakdown after SIMD coarse optimization
The benchmark output SHALL continue to expose a separate coarse breakdown after the centroid score kernel is optimized, so that coarse score gains can be isolated from query normalize and top-n selection.

#### Scenario: Coarse timing fields remain available after optimization
- **WHEN** `bench_e2e` runs on a build that includes the SIMD coarse centroid score kernel
- **THEN** the result output SHALL still include `avg_coarse_select_ms`
- **AND** it SHALL still include `avg_coarse_score_ms`
- **AND** it SHALL still include `avg_coarse_topn_ms`

#### Scenario: Benchmark can compare scalar and SIMD coarse score paths
- **WHEN** the coarse score implementation changes from scalar to SIMD
- **THEN** the benchmark output SHALL remain sufficient to compare optimization before and after under the same dataset and query parameters
- **AND** the comparison SHALL not require removing the existing coarse timing fields
