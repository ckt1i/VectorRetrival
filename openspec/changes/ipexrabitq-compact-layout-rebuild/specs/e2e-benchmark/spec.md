## ADDED Requirements

### Requirement: E2E benchmark SHALL report compact-layout rebuild identity
The E2E benchmark output SHALL make it explicit whether a run used the legacy `v10 packed_sign` Stage2 layout or the rebuild-required `v11` compact layout.

#### Scenario: Benchmark output distinguishes `v10` and `v11`
- **WHEN** benchmark results are exported for Stage2 layout comparison
- **THEN** the output SHALL identify whether the index uses `v10 packed_sign` or `v11 compact layout`
- **AND** that identity SHALL be stable enough for before/after comparison

### Requirement: E2E benchmark SHALL compare compact-layout serving against the current low-overhead baseline
The benchmark workflow SHALL support direct comparison between the current low-overhead serving baseline and the rebuilt compact-layout serving path under the same query parameters.

#### Scenario: Same operating point is run before and after compact rebuild
- **WHEN** the compact layout change is evaluated
- **THEN** benchmark runs SHALL use the same dataset, `nlist`, `nprobe`, `topk`, query count, preload mode, resident mode, and CRC settings before and after rebuild
- **AND** the exported results SHALL remain directly comparable

### Requirement: E2E benchmark SHALL expose Stage2 benefit after compact rebuild
The benchmark output SHALL continue to expose Stage2 and end-to-end breakdown fields after compact rebuild, so that the effect of the new layout can be attributed.

#### Scenario: Stage2 and E2E fields remain available after compact rebuild
- **WHEN** benchmark runs on a `v11` compact index
- **THEN** the output SHALL still include `avg_query_ms`
- **AND** it SHALL still include `avg_probe_stage2_ms`
- **AND** it SHALL still include the existing low-overhead query-path timing fields needed to explain the result
