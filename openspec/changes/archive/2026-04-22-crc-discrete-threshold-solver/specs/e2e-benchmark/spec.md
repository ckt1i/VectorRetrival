## ADDED Requirements

### Requirement: E2E benchmark SHALL expose runtime CRC solver selection
The runtime CRC preparation path in `bench_e2e` MUST allow explicit selection of the CRC calibration solver so that `brent` and `discrete_threshold` can be compared under the same serving operating point. The benchmark default SHALL remain `brent`.

#### Scenario: Benchmark runs without a solver override
- **WHEN** `bench_e2e` runs with CRC enabled and no solver override is provided
- **THEN** it MUST use `brent` as the runtime calibration solver

#### Scenario: Benchmark runs with a discrete solver override
- **WHEN** `bench_e2e` runs with CRC enabled and `discrete_threshold` is selected
- **THEN** it MUST run runtime CRC calibration with the discrete solver
- **AND** it MUST report that solver choice in the benchmark output

### Requirement: E2E benchmark SHALL export CRC solver provenance fields
When runtime CRC calibration is performed, the benchmark output MUST include the solver provenance and internal cost fields needed to compare Brent and discrete threshold solving under the same index and query settings.

#### Scenario: Runtime CRC provenance is written to results
- **WHEN** `bench_e2e` completes a run that performs runtime CRC calibration
- **THEN** the exported result MUST include `crc_solver`
- **AND** it MUST include `crc_candidate_count`
- **AND** it MUST include `crc_solver_ms`
- **AND** it MUST include `crc_profile_build_ms`
- **AND** it MUST include `crc_objective_evals`
