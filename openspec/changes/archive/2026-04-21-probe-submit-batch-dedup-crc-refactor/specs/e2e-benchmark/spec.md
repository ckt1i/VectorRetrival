## MODIFIED Requirements

### Requirement: E2E optimization validation SHALL use a stable dual-benchmark protocol
Any follow-up query optimization SHALL be validated with both a full E2E run and a query-only perf run under the same operating point, so that CPU hotspot movement and end-to-end impact remain jointly observable.

#### Scenario: Optimization round is measured with both benchmark modes
- **WHEN** a follow-up optimization item is evaluated
- **THEN** the evaluation SHALL include a full E2E measurement under the fixed operating point
- **AND** it SHALL include a query-only perf measurement under the same query parameters

#### Scenario: `probe_submit` refactor keeps submit-path fields comparable
- **WHEN** the `probe_submit` refactor is benchmarked
- **THEN** benchmark output SHALL continue to report submit-related timing and candidate-count fields needed for before/after comparison
- **AND** those fields SHALL remain sufficient to compare `probe_submit_ms`, `uring_prep_ms`, duplicate-related counts, and unique-fetch-related counts across optimization stages
