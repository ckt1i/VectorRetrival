## ADDED Requirements

### Requirement: Benchmarking SHALL measure the packed-sign kernel route before batch boosting
The benchmark workflow SHALL evaluate the packed-sign dedicated Stage2 kernel route before Stage2 batch boosting is considered for implementation.

#### Scenario: Packed-sign kernel route gets a standalone benchmark pass
- **WHEN** this change is evaluated
- **THEN** the benchmark workflow SHALL include a standalone measurement pass for the packed-sign dedicated Stage2 kernel route before any batch boosting result is used for decision making

### Requirement: Benchmarking SHALL distinguish kernel gains from Stage2 structure gains
The benchmark workflow SHALL preserve enough output and profiling context to distinguish gains from the packed-sign kernel route and gains from the Stage2 batch boosting route.

#### Scenario: Perf result can be attributed to one route
- **WHEN** clean perf is collected for this change
- **THEN** the result SHALL remain sufficient to attribute whether the observed improvement came primarily from the Stage2 kernel path or from the Stage2 batch boosting structure

### Requirement: This change SHALL continue to use clean perf and full E2E together
The benchmark workflow for this change SHALL continue to use both clean perf and full E2E, so that Stage2 compute gains and end-to-end serving gains can both be judged.

#### Scenario: Both benchmark modes are produced
- **WHEN** a route in this change is evaluated
- **THEN** the evaluation SHALL include query-only clean perf
- **AND** it SHALL include the corresponding full E2E run under the formal serving parameters
