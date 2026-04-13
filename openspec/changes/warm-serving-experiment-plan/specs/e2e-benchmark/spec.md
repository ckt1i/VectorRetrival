## MODIFIED Requirements

### Requirement: E2E benchmark output SHALL support warm Pareto analysis
The E2E benchmark workflow SHALL support warm steady-state reporting as the primary analysis mode and SHALL produce output that is sufficient for recall-latency Pareto comparison and mechanism attribution.

#### Scenario: Warm operating-point sweep is reported
- **WHEN** the benchmark is run for the main post-baseline experiment phase
- **THEN** it SHALL report multiple BoundFetch operating points under the same warm protocol
- **AND** each point SHALL include `recall@10` and `e2e_ms`

#### Scenario: Mechanism fields are preserved for attribution
- **WHEN** a BoundFetch warm result is exported
- **THEN** the benchmark output SHALL preserve mechanism-level fields needed for attribution
- **AND** those fields SHALL include the effective submission and probe breakdown needed to explain the result

#### Scenario: Appendix validation is separated from the main result
- **WHEN** queue-depth, submission mode, or SQPOLL checks are run
- **THEN** they SHALL be treated as supporting validation rather than the main benchmark objective
- **AND** the main result SHALL remain the warm recall-latency Pareto comparison
