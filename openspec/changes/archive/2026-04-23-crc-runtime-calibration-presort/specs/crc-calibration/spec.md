## ADDED Requirements

### Requirement: CRC calibration SHALL precompute query stop profiles for repeated lambda evaluation
CRC calibration MUST support a profile-based evaluation path in which each calibration query is transformed once into a reusable stop profile before repeated `lambda` evaluation begins. The stop profile SHALL preserve the existing stop semantics by representing the same `(reg_score, step)` ordering that the current implementation uses during `ComputePredictions`.

#### Scenario: One query is evaluated across multiple lambda candidates
- **WHEN** `PickLambdaReg` or Brent evaluates multiple candidate `lambda` values for the same query set
- **THEN** the system MUST reuse a precomputed per-query stop profile instead of rebuilding and resorting the query's `(reg_score, step)` pairs for each evaluation

#### Scenario: Binary search selects the same stop step as the legacy sorted scan
- **WHEN** a `lambda` value is evaluated against a precomputed stop profile
- **THEN** the selected stop step MUST match the step that would have been chosen by the legacy "sort then take the last `reg_score <= lambda`" rule

### Requirement: CRC calibration SHALL support table-driven FNR accumulation during Brent iterations
During Brent-based `lambda` solving, CRC calibration MUST allow `FNR(lambda)` to be computed through lightweight lookup and accumulation over precomputed query statistics, rather than rebuilding prediction containers on every iteration. Any precomputed hit-count or overlap table used for this path MUST remain equivalent to the overlap that the legacy implementation would have computed from the selected step's predictions and the query's GT set.

#### Scenario: Brent evaluates lambda without rebuilding prediction sets
- **WHEN** Brent invokes the calibration objective for a candidate `lambda`
- **THEN** the system MUST be able to compute the query contribution to `FNR(lambda)` using the selected stop step plus precomputed overlap or equivalent hit-count statistics
- **AND** it MUST NOT require per-iteration reconstruction of full prediction-set containers for every query

#### Scenario: Profile-based FNR matches legacy calibration behavior
- **WHEN** calibration runs on the same `crc_scores` input, split, and solver configuration before and after the profile-based optimization
- **THEN** the resulting `lamhat`, `actual_fnr`, and `avg_probed` MUST remain equivalent or differ only within implementation-level numeric tolerance

### Requirement: First-step CRC calibration optimization SHALL preserve the Brent solver contract
The first-step CRC calibration optimization MUST preserve the current Brent-based solving contract and SHALL NOT replace it with discrete candidate threshold search. The optimization MUST therefore be limited to reducing repeated work inside `FNR(lambda)` evaluation while keeping `PickLambdaReg`, Brent root finding, and `CalibrationResults` semantics unchanged.

#### Scenario: First-step optimization is enabled
- **WHEN** the optimized calibration path is used
- **THEN** the system MUST continue to solve `lamhat` with the existing Brent-based workflow
- **AND** it MUST continue to emit the same `CalibrationResults` fields as the legacy path

#### Scenario: Discrete threshold search remains out of scope
- **WHEN** this change is implemented
- **THEN** the system MUST NOT silently switch calibration from Brent solving to discrete candidate threshold search as part of the same optimization step
