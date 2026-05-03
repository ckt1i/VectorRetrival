## ADDED Requirements

### Requirement: E2E benchmark SHALL support thesis canonical-index reuse mode
For the thesis minimal main-sweep workflow, `bench_e2e` SHALL be able to execute against a previously built canonical index directory instead of rebuilding the dataset index inline.

#### Scenario: Benchmark runs with a prebuilt canonical artifact
- **WHEN** `bench_e2e` is invoked for the thesis main-sweep with `--index-dir`
- **THEN** it SHALL skip the inline build phase and execute against the provided canonical artifact directory
- **AND** it SHALL keep the dataset, builder, and operating-point metadata visible in the exported result

#### Scenario: Missing canonical artifact fails explicitly
- **WHEN** `bench_e2e` is invoked for the thesis main-sweep with a missing or invalid canonical artifact directory
- **THEN** the benchmark SHALL fail explicitly
- **AND** it SHALL NOT silently rebuild a replacement index from the raw vectors in that same run

### Requirement: E2E benchmark outputs SHALL record canonical-artifact provenance for thesis sweeps
For thesis canonical-index runs, benchmark outputs SHALL expose enough provenance to distinguish direct artifact reuse from inline build execution.

#### Scenario: Provenance fields are exported for canonical-index runs
- **WHEN** `bench_e2e` finishes a thesis run that consumed a canonical artifact
- **THEN** the exported metadata SHALL record the canonical artifact directory
- **AND** it SHALL record the effective coarse builder identity used to produce that artifact
- **AND** it SHALL make the run distinguishable from an inline-build run

### Requirement: E2E benchmark runs SHALL support thesis warmup and strict-measurement separation
For thesis main-sweep use, `bench_e2e` invocations SHALL be usable in a two-step protocol where warmup is separate from the formal measurement result.

#### Scenario: Warmup output is marked non-authoritative
- **WHEN** `bench_e2e` is invoked as a thesis warmup run
- **THEN** its output SHALL be written outside the formal thesis measurement result path or marked so it cannot be selected as the main result
- **AND** it SHALL preserve enough metadata to verify it used the same canonical artifact and operating point as the following measurement run

#### Scenario: Strict measurement keeps external GT active
- **WHEN** `bench_e2e` is invoked as a thesis formal measurement run
- **THEN** it SHALL run with `skip_gt=0`
- **AND** a provided `--gt-file` SHALL be consumed for recall calculation rather than recorded as unused
- **AND** exported results SHALL report `recall_available=true` when the approved GT file is present and valid
