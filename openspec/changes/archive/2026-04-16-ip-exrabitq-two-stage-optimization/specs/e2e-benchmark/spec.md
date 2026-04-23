## MODIFIED Requirements

### Requirement: Benchmark Must Support Clean Perf and Full E2E As Separate Truths
The benchmark system SHALL treat clean perf and full E2E as separate but equally required evaluation modes for query optimization work.

#### Scenario: Clean perf isolates query CPU hotspots
- **WHEN** benchmark runs in query-only mode with `fine_grained_timing=0`
- **THEN** it SHALL provide a hotspot profile that is not materially polluted by fine-grained timing instrumentation in prepare sub-stages

#### Scenario: Full E2E remains the end-to-end truth
- **WHEN** benchmark runs in the standard full E2E mode
- **THEN** it SHALL continue to report recall and end-to-end latency fields required to judge whether a kernel optimization improves real search latency

### Requirement: Benchmark Must Support Stage2 Optimization Decisions
The benchmark system SHALL provide enough information to decide whether `IPExRaBitQ` optimization should stop at Stage 1 or continue into storage-layout changes.

#### Scenario: Stage 1 decision uses clean perf
- **WHEN** Stage 1 `IPExRaBitQ` kernel work is completed
- **THEN** clean perf results SHALL be sufficient to determine whether `IPExRaBitQ` remains a significant hotspot

#### Scenario: Stage 2 decision also checks end-to-end latency
- **WHEN** clean perf shows kernel improvement
- **THEN** full E2E results SHALL also be used to determine whether further storage-layout work is still justified
