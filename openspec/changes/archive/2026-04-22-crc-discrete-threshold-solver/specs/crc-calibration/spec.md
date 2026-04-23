## ADDED Requirements

### Requirement: CRC calibration SHALL support selectable solver modes
CRC calibration MUST support explicit solver selection between `brent` and `discrete_threshold`. The default solver SHALL remain `brent` until a later change explicitly switches the default. Both solver modes MUST reuse the same profile-based `lambda` evaluation path rather than maintaining separate FNR evaluation semantics.

#### Scenario: Benchmark selects the default solver
- **WHEN** CRC calibration is invoked without an explicit solver override
- **THEN** the system MUST use `brent` as the calibration solver

#### Scenario: Benchmark selects the discrete solver
- **WHEN** CRC calibration is invoked with `discrete_threshold`
- **THEN** the system MUST solve `lamhat` using discrete candidate threshold search
- **AND** it MUST continue to use the same `StopProfiles` and `EvaluateLambda` semantics used by the Brent path

### Requirement: Discrete threshold search SHALL operate on actual profile breakpoints
The `discrete_threshold` solver MUST derive its candidate `lambda` set from the actual `reg_score` breakpoint values stored in the active `StopProfiles`. It MUST sort and deduplicate those real breakpoint values before search, and MUST NOT silently replace them with a fixed-step grid or other coarse sampling strategy.

#### Scenario: Candidate thresholds are collected for discrete solving
- **WHEN** the discrete solver prepares its candidate `lambda` set
- **THEN** it MUST collect the candidate thresholds from the real `sorted_scores` values present in the active stop profiles
- **AND** it MUST sort and deduplicate them before evaluating candidates

#### Scenario: No legal candidate satisfies the FNR target
- **WHEN** the discrete solver finds no candidate with `FNR <= target_fnr`
- **THEN** it MUST return the most conservative boundary candidate
- **AND** it MUST NOT silently fall back to Brent solving

### Requirement: Discrete threshold search SHALL minimize avg_probed among legal candidates
For the `discrete_threshold` solver, the selected `lamhat` MUST be the legal candidate with the minimum `avg_probed` among all candidates satisfying `FNR <= target_fnr`. If multiple legal candidates tie on `avg_probed`, the solver SHALL choose the larger `lambda`.

#### Scenario: Multiple legal candidates exist
- **WHEN** more than one candidate satisfies the FNR constraint
- **THEN** the system MUST select the candidate with the smallest `avg_probed`
- **AND** if multiple legal candidates share the same `avg_probed`, it MUST select the larger `lambda`

### Requirement: Brent SHALL remain the default solver until comparison criteria are met
Introducing `discrete_threshold` MUST NOT by itself change the default CRC calibration solver. The default SHALL remain `brent` until a later change explicitly decides to switch the default based on comparison results.

#### Scenario: New solver support is introduced
- **WHEN** the system adds support for `discrete_threshold`
- **THEN** `brent` MUST remain the default calibration solver unless a later change explicitly updates that default

### Requirement: CRC solver provenance SHALL be observable
CRC calibration and benchmark outputs MUST make the active solver and its internal cost visible. The exported output MUST include solver identity and enough statistics to distinguish profile-build cost from solver-loop cost.

#### Scenario: CRC benchmark exports solver provenance
- **WHEN** a benchmark run performs runtime CRC calibration
- **THEN** the result output MUST include `crc_solver`
- **AND** it MUST include `crc_solver_ms`
- **AND** it MUST include `crc_profile_build_ms`
- **AND** it MUST include solver-specific search statistics such as `crc_candidate_count` or `crc_objective_evals`
