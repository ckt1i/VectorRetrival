## MODIFIED Requirements

### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL support direct before/after comparison between the existing `full_preload` query path and the new resident cluster warm path under the same warm-serving query settings. Preload-related startup cost SHALL be reported explicitly and SHALL NOT be folded into steady-state `e2e_ms`.

#### Scenario: Same operating point is run under both resident variants
- **WHEN** the benchmark evaluates the resident cluster warm optimization
- **THEN** it SHALL run the same dataset and search parameters under both the baseline `full_preload` path and the new resident warm path
- **AND** the exported results SHALL make the resident mode explicit

#### Scenario: Query-speed comparison fields are exported
- **WHEN** a resident warm comparison result is exported
- **THEN** the result SHALL include `recall@10`, `e2e_ms`, and `p99_ms`
- **AND** it SHALL include enough metadata to reconstruct the exact operating point being compared

#### Scenario: Resident preload cost is recorded separately from steady-state query speed
- **WHEN** resident cluster warm mode is benchmarked
- **THEN** the benchmark output SHALL record `clu_preload_time_ms`, `clu_preload_bytes`, and `resident_cluster_mem_bytes`
- **AND** steady-state `e2e_ms` SHALL exclude the preload stage

#### Scenario: Before/after speedup can be computed from one benchmark campaign
- **WHEN** the benchmark finishes a resident warm comparison campaign
- **THEN** it SHALL export enough fields to compute the speedup of resident warm mode over the baseline `full_preload` path
- **AND** both runs SHALL use the same `dataset`, `queries`, `nlist`, `nprobe`, `topk`, `crc-alpha`, and `epsilon-percentile`
