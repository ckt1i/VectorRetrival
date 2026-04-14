## ADDED Requirements

### Requirement: Full `.clu` preload SHALL be supported as a query-time cluster-data mode
The system SHALL support a mode that reads the entire `.clu` file before query execution and keeps the cluster-side quantized-vector data resident in memory for reuse across queries.

#### Scenario: Preload succeeds before query execution
- **WHEN** full `.clu` preload mode is enabled and the index is opened or warmed up
- **THEN** the system SHALL read the complete `.clu` file before executing the measured query batch
- **AND** the resulting resident state SHALL remain available for subsequent queries in that run

### Requirement: Resident cluster views SHALL be materialized during preload
The preload path SHALL materialize per-cluster resident views that are sufficient for probe-time access to quantized codes and address-related metadata without issuing new `.clu` reads.

#### Scenario: Resident cluster view is available for a probed cluster
- **WHEN** a query probes a cluster after full `.clu` preload has completed
- **THEN** the probe path SHALL obtain that cluster's codes and address-related metadata from resident memory
- **AND** it SHALL NOT require a fresh `.clu` block read for that cluster

### Requirement: Full preload SHALL preserve search semantics
Switching from sliding-window `.clu` reads to full preload SHALL NOT change the search semantics of the same operating point.

#### Scenario: Same search parameters produce the same result semantics
- **WHEN** the benchmark is run with the same dataset and search parameters under `window` mode and `full_preload` mode
- **THEN** both runs SHALL use the same `nprobe`, `crc-alpha`, `epsilon-percentile`, `nlist`, and `topk`
- **AND** any observed performance difference SHALL come from the `.clu` loading strategy rather than a changed search configuration

### Requirement: Payload I/O SHALL remain on the normal query path
The full `.clu` preload mode SHALL preload only cluster-side quantized-vector and address-related state, while leaving payload or raw-vector body reads on the existing query pipeline.

#### Scenario: Payload bytes are not preloaded by cluster preload
- **WHEN** full `.clu` preload mode is enabled
- **THEN** the system SHALL continue to fetch payload or raw-vector bodies through the normal query-time path
- **AND** `.clu` preload SHALL NOT be treated as a full payload cache
