## ADDED Requirements

### Requirement: Probe submit SHALL use a batch-oriented submission path
The probe submit path SHALL consume each `CandidateBatch` through a batch-oriented submission flow rather than invoking a full per-candidate submission routine for every surviving candidate.

#### Scenario: Candidate batch is processed as a submission batch
- **WHEN** `AsyncIOSink::OnCandidates(batch)` receives a non-empty `CandidateBatch`
- **THEN** the implementation SHALL scan and partition the batch before issuing reads
- **AND** it SHALL prepare slot, buffer, and read submission work from that batch-oriented state rather than immediately issuing a full standalone submission routine for each candidate

### Requirement: Probe submit SHALL preserve `VEC_ALL` and `VEC_ONLY` selection semantics
The batched submission path SHALL preserve the current `SafeIn + size-threshold -> VEC_ALL` behavior and the current `VEC_ONLY` behavior for all other candidates.

#### Scenario: SafeIn candidate still uses current `VEC_ALL` rule
- **WHEN** a candidate is classified as `SafeIn`
- **AND** its address size is within the configured `safein_all_threshold`
- **THEN** the batched submission path SHALL submit it as `VEC_ALL`

#### Scenario: Non-`VEC_ALL` candidate still uses `VEC_ONLY`
- **WHEN** a candidate does not satisfy the current `VEC_ALL` rule
- **THEN** the batched submission path SHALL submit it as `VEC_ONLY`

### Requirement: Probe submit SHALL use batch-aware dedup
The probe submit path SHALL perform dedup in a batch-aware manner, including local filtering within the current candidate batch and query-level dedup across the current query.

#### Scenario: Batch-local duplicate is filtered before global dedup
- **WHEN** two or more candidates in the same `CandidateBatch` map to the same address offset
- **THEN** the submission path SHALL keep only one candidate for further query-level dedup and submission
- **AND** the duplicate accounting SHALL remain visible to statistics

#### Scenario: Query-global duplicate is not submitted twice
- **WHEN** a candidate survives batch-local filtering but its address offset has already been submitted earlier in the same query
- **THEN** the submission path SHALL NOT submit another read for that offset

### Requirement: CRC estimate updates SHALL occur no later than cluster end
When CRC is enabled, the submission path SHALL NOT require per-candidate heap updates on the hot path, but it SHALL make the current cluster's estimate contributions visible before the next cluster-level early-stop decision.

#### Scenario: Cluster-local estimates are merged before early-stop check
- **WHEN** a probed cluster finishes submission-path processing
- **THEN** any CRC estimates collected from that cluster SHALL be merged into the query-level CRC state before the cluster-level early-stop check is evaluated

### Requirement: Submit-path timing SHALL use batch or group boundaries
The submission path SHALL expose submit-related timing using batch- or group-level boundaries rather than per-candidate micro-timing in the hot path.

#### Scenario: Submit timing remains attributable without per-candidate timers
- **WHEN** the batched submit path records timing
- **THEN** it SHALL preserve `probe_submit_ms` comparability
- **AND** it SHALL preserve `uring_prep_ms` comparability
- **AND** it SHALL NOT require per-candidate timer calls on the hot path
