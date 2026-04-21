## MODIFIED Requirements

### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL support direct before/after comparison between the existing sliding-window `.clu` loading path and the new full-preload `.clu` loading path under the same warm-serving query settings, and it SHALL also preserve enough coarse-builder provenance to distinguish historical Python-precomputed Faiss clustering from in-process Faiss clustering.

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

#### Scenario: Faiss coarse-builder source is distinguishable in benchmark output
- **WHEN** the benchmark runs with `coarse_builder=faiss_kmeans`
- **THEN** the exported config and summary outputs SHALL distinguish whether clustering came from precomputed artifacts or in-process automatic training
- **AND** they SHALL record the effective coarse metric used by the built index
- **AND** they SHALL not label the resulting clustering source as a SuperKMeans-derived mode
