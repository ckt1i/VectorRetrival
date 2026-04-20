## MODIFIED Requirements

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
