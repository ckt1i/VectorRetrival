## MODIFIED Requirements

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
