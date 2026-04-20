## ADDED Requirements

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
