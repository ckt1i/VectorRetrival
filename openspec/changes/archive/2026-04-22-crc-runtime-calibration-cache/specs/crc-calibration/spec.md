## ADDED Requirements

### Requirement: CRC calibration SHALL reuse subset-level step statistics across reg-lambda evaluation
CRC calibration MUST support a subset-level static cache for query-step statistics that do not depend on `reg_lambda`, including GT-size-equivalent information and step-level overlap or equivalent hit-count statistics. The cache SHALL be reusable across calib, tune, and test evaluation for a fixed subset without recomputing overlap from prediction IDs on every profile build.

#### Scenario: Multiple reg-lambda candidates reuse the same subset statistics
- **WHEN** calibration evaluates multiple `reg_lambda` candidates for the same calib or tune subset
- **THEN** the system MUST reuse precomputed subset-level step statistics instead of recomputing step-level overlap from prediction IDs for each candidate

#### Scenario: Profile build projects overlap from static cache
- **WHEN** a reg-lambda-specific stop profile is built
- **THEN** the overlap contribution used by that profile MUST be derived from the subset-level static cache via the selected step mapping
- **AND** it MUST remain equivalent to the overlap that would have been computed directly from the selected step's predictions and GT

### Requirement: CRC calibration SHALL precompute rank for fixed nonconformity inputs
For a fixed subset and normalization result, CRC calibration MUST support a precomputed rank view of `nonconf(q, p)` so that `RegularizeScores` or its equivalent reg-score path does not need to re-sort the same `nonconf` inputs for every `reg_lambda` candidate.

#### Scenario: Repeated reg-lambda evaluation does not re-sort nonconf
- **WHEN** calibration evaluates multiple `reg_lambda` values on the same normalized query subset
- **THEN** the system MUST reuse a precomputed rank-by-step view of `nonconf`
- **AND** it MUST NOT require a fresh per-query `nonconf` sort for each `reg_lambda`

#### Scenario: Rank-precomputed regularization preserves legacy behavior
- **WHEN** the optimized regularization path is run on the same normalized inputs as the legacy implementation
- **THEN** the resulting reg-score ordering and downstream stop-step behavior MUST remain equivalent or differ only within implementation-level numeric tolerance

### Requirement: Second-step CRC calibration optimization SHALL preserve the profile-based Brent contract
The second-step CRC calibration optimization MUST continue to use the existing profile-based Brent solving contract introduced by the presort optimization. It SHALL reduce repeated cache construction and repeated sorting work inside preparation, but SHALL NOT replace Brent root solving or change the semantics of `CalibrationResults`.

#### Scenario: Cache optimization is enabled
- **WHEN** the cache-optimized calibration path is used
- **THEN** the system MUST continue to solve `lamhat` through the existing Brent-based workflow
- **AND** it MUST continue to emit the same `CalibrationResults` fields as before

#### Scenario: Discrete threshold search remains out of scope for cache optimization
- **WHEN** this cache-oriented optimization step is implemented
- **THEN** the system MUST NOT silently change the calibration solver from Brent to discrete candidate threshold search as part of the same change
