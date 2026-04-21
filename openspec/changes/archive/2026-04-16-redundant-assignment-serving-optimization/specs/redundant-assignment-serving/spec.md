## ADDED Requirements

### Requirement: IVF builder SHALL support top-2 redundant assignment for warm serving
The IVF build path SHALL support a redundant assignment mode in which each database vector is assigned to the two nearest coarse clusters. The builder SHALL preserve a stable original vector identity across duplicated postings and SHALL keep single-assignment behavior available as the default mode.

#### Scenario: Single-assignment mode remains the default
- **WHEN** the builder is run without enabling redundant assignment
- **THEN** each database vector SHALL be written to exactly one coarse cluster
- **AND** the resulting index SHALL remain compatible with the existing single-assignment query path

#### Scenario: Top-2 redundant assignment writes two postings per vector
- **WHEN** the builder is run with redundant assignment factor `2`
- **THEN** each database vector SHALL be assigned to its two nearest coarse clusters
- **AND** the two postings SHALL share the same original vector identity and payload address semantics

### Requirement: Query execution SHALL return unique original vectors under duplicated postings
The serving path SHALL treat duplicated postings as alternate access paths to the same original vector. Final search results SHALL be unique by original vector identity, even when the same vector is encountered in multiple probed clusters.

#### Scenario: Final top-k contains no duplicate vector identities
- **WHEN** a query probes multiple clusters that contain duplicated postings for the same vector
- **THEN** the final `topk` result SHALL contain that vector at most once
- **AND** recall evaluation SHALL observe the original vector identity rather than posting multiplicity

#### Scenario: Duplicate postings do not create duplicate payload semantics
- **WHEN** the same original vector appears through multiple postings during one query
- **THEN** payload and raw-vector semantics SHALL remain bound to the original vector record
- **AND** the query path SHALL not expose duplicate user-visible records

### Requirement: Query execution SHALL deduplicate before raw-vector fetch submission
The serving path SHALL collapse duplicate candidates before submitting raw-vector or payload reads for reranking, so that duplicated postings do not multiply downstream I/O and rerank work.

#### Scenario: Duplicate candidates collapse before raw-vector fetch
- **WHEN** two or more probed clusters emit candidates for the same original vector during one query
- **THEN** the scheduler SHALL submit at most one raw-vector fetch for that vector
- **AND** duplicate postings SHALL be counted as deduplicated rather than independently reranked

#### Scenario: Deduplication preserves best candidate for rerank
- **WHEN** duplicate postings for the same original vector produce different coarse-stage scores
- **THEN** the query path SHALL preserve a deterministic representative for downstream rerank
- **AND** the final result SHALL remain unique by original vector identity

### Requirement: Redundant-assignment serving SHALL be observable and reversible
The index metadata and query output SHALL expose whether redundant assignment is enabled, so that serving runs can be compared fairly against single-assignment baselines and reverted without ambiguity.

#### Scenario: Index metadata records assignment mode
- **WHEN** an index is built with redundant assignment enabled
- **THEN** the index metadata SHALL record the assignment mode and assignment factor
- **AND** benchmark or diagnostic tools SHALL be able to distinguish it from a single-assignment index

#### Scenario: Serving configuration can fall back to single-assignment behavior
- **WHEN** a run uses a single-assignment index or disables redundant assignment
- **THEN** the query path SHALL execute without duplicate-candidate semantics
- **AND** existing single-assignment behavior SHALL remain available as the rollback path
