## ADDED Requirements

### Requirement: Warm serving benchmark SHALL compare redundant assignment against both current 2048 and 1024 references
The warm serving benchmark plan SHALL treat `2048 + top-2 redundant assignment` as an explicit operating line and SHALL compare it under the same protocol against the current `2048` single-assignment baseline and the `1024` single-assignment reference.

#### Scenario: Three-line warm comparison is preserved
- **WHEN** the main redundant-assignment benchmark is executed
- **THEN** it SHALL report results for `2048` single-assignment, `2048 + top-2 redundant assignment`, and `1024` single-assignment
- **AND** the three lines SHALL use the same dataset, query count, top-k, warmup mode, and reporting protocol

#### Scenario: High-recall comparison remains the primary decision point
- **WHEN** the benchmark is used to evaluate whether redundant assignment is worth keeping
- **THEN** the reported results SHALL make the `0.99+ recall` region directly comparable across the three lines
- **AND** the decision SHALL not rely on coarse-coverage-only offline analysis

### Requirement: Warm serving output SHALL expose duplication and deduplication cost
The benchmark output SHALL include the fields needed to explain the benefit and cost of redundant assignment beyond recall and latency alone.

#### Scenario: Assignment mode is recorded in aggregate output
- **WHEN** an operating point is written to the aggregate warm result
- **THEN** the output SHALL record whether the index used single assignment or redundant assignment
- **AND** the assignment factor SHALL be recoverable from the exported fields

#### Scenario: Duplicate-candidate cost is reported
- **WHEN** a redundant-assignment operating point is exported
- **THEN** the output SHALL include duplication-related fields sufficient to distinguish probe expansion from deduplicated downstream work
- **AND** those fields SHALL include at least one measure before deduplication and one measure after deduplication

#### Scenario: Index and preload cost is reported
- **WHEN** a redundant-assignment operating point is exported
- **THEN** the output SHALL include index-size or preload-cost fields needed to judge system overhead
- **AND** those fields SHALL be comparable to the single-assignment baselines

### Requirement: The benchmark SHALL define a stop/keep decision for redundant assignment
The benchmark workflow SHALL produce enough information to decide whether redundant assignment remains in scope after the first warm serving comparison.

#### Scenario: Keep redundant assignment only if it improves the high-recall serving trade-off
- **WHEN** `2048 + top-2 redundant assignment` reaches the target high-recall region
- **THEN** the benchmark output SHALL make it possible to determine whether it preserves a meaningful latency advantage over `1024` single-assignment
- **AND** if that advantage is not preserved, the direction SHALL be considered a stop candidate rather than an automatic next step
