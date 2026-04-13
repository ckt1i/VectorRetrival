## ADDED Requirements

### Requirement: bench_e2e SHALL expose submit-path control parameters
`bench_e2e` SHALL expose CLI parameters needed to evaluate submit-path optimizations, including queue depth and whether SQPOLL is enabled. It SHALL also expose a way to select the submission isolation mode used by the scheduler so shared and isolated configurations can be compared under the same workload.

#### Scenario: Queue depth is passed from CLI
- **WHEN** `bench_e2e` is run with `--io-queue-depth 256`
- **THEN** the search configuration used for the run SHALL record queue depth 256
- **AND** the reader initialization SHALL use that queue depth

#### Scenario: SQPOLL mode is passed from CLI
- **WHEN** `bench_e2e` is run with SQPOLL enabled
- **THEN** the benchmark SHALL attempt to initialize the query reader in SQPOLL mode
- **AND** the output SHALL record whether SQPOLL was requested and whether it became the effective mode

#### Scenario: Submission isolation mode is selectable
- **WHEN** `bench_e2e` is run with a submission mode selector
- **THEN** the benchmark SHALL configure the scheduler to run in the requested shared or isolated mode
- **AND** the selected mode SHALL be included in the output metadata

### Requirement: bench_e2e SHALL report submit-path timing and configuration
`bench_e2e` SHALL report enough timing and configuration data to evaluate submit-path optimizations without relying on ad hoc code inspection. The output SHALL include effective queue depth, effective submission mode, and submit-related timing counters for each run.

#### Scenario: Timing output includes submit-path metrics
- **WHEN** a benchmark run completes
- **THEN** the output SHALL include `uring_submit_ms`
- **AND** it SHALL include at least one submit-path configuration field such as queue depth, SQPOLL mode, or submission isolation mode

#### Scenario: Sweep output preserves comparability
- **WHEN** `bench_e2e` runs in sweep mode across multiple `nprobe` values
- **THEN** each row or result entry SHALL preserve the submit-path configuration for that run
- **AND** submit-path timing fields SHALL remain comparable across rows
