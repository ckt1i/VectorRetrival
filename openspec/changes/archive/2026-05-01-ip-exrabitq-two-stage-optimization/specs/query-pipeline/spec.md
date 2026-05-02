## MODIFIED Requirements

### Requirement: Query Pipeline Must Expose Stable Hotspot Boundaries
The query pipeline SHALL expose stable timing and profiling boundaries for coarse selection, prepare, Stage 1, Stage 2, submit, and rerank so that optimization work can be evaluated without conflating multiple segments.

#### Scenario: Fine-grained timing reports stage boundaries
- **WHEN** fine-grained timing is enabled for benchmark analysis
- **THEN** the system SHALL continue to report stable prepare, Stage 1, Stage 2, and submit boundaries that can be compared across optimization rounds

#### Scenario: Clean perf excludes fine-grained timing pollution
- **WHEN** clean perf is run with `fine_grained_timing=0`
- **THEN** the query pipeline SHALL not execute prepare sub-stage timing code that materially pollutes the query CPU hotspot profile

### Requirement: Stage 2 Optimization Shall Preserve Query Semantics
The query pipeline SHALL preserve query semantics when optimizing Stage 2 `IPExRaBitQ`, including candidate ordering behavior, SafeIn/SafeOut semantics, and downstream rerank eligibility.

#### Scenario: Stage 2 kernel replacement preserves classification behavior
- **WHEN** the Stage 2 kernel is replaced by an optimized implementation
- **THEN** the resulting Stage2 distance estimates and classification outcomes SHALL remain compatible with the existing query pipeline semantics

#### Scenario: Later layout upgrades remain pipeline-compatible
- **WHEN** a later stage introduces `sign bit-pack` or compact ex-code layout
- **THEN** the query pipeline SHALL continue to present an equivalent Stage2 query contract to `ClusterProber` and downstream consumers
