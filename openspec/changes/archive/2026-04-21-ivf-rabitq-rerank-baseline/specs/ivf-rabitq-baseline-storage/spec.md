## ADDED Requirements

### Requirement: The IVF-RaBitQ baseline SHALL persist a three-plane storage layout
The baseline SHALL separate compressed search data, raw vectors, and payload data so that candidate generation, exact rerank, and payload fetch can be measured independently.

#### Scenario: Compressed search data is stored independently
- **WHEN** the baseline index build completes
- **THEN** it SHALL emit a compressed index plane containing `meta.json`, `coarse_centroids.bin`, `cluster_offsets.bin`, and `clusters.clu`
- **AND** that compressed index plane SHALL be sufficient to run coarse probing and compressed candidate scoring without reading payload data

#### Scenario: Raw vectors are stored independently
- **WHEN** the baseline index build completes
- **THEN** it SHALL emit a raw vector plane as a standalone file
- **AND** the raw vector plane SHALL be readable by `row_id` without parsing `clusters.clu`

#### Scenario: Payload remains external to the baseline core index
- **WHEN** the baseline is integrated into a coupled E2E benchmark
- **THEN** payload fetch SHALL remain an external backend concern
- **AND** the baseline core index SHALL NOT require payload bytes to be embedded inside `clusters.clu`

### Requirement: The compressed cluster file SHALL expose stable per-cluster blocks
The baseline SHALL store RaBitQ-compressed posting data in a cluster-organized file that can be addressed by cluster id.

#### Scenario: Cluster offsets locate each cluster block
- **WHEN** a search run probes one or more coarse clusters
- **THEN** the baseline SHALL use `cluster_offsets.bin` or an equivalent index to locate the byte range of each cluster block inside `clusters.clu`
- **AND** the cluster block lookup SHALL NOT require scanning unrelated clusters

#### Scenario: Each cluster block carries the minimum required record data
- **WHEN** a cluster block is decoded for candidate scoring
- **THEN** it SHALL provide the record count, record row ids, RaBitQ codes, and any required side statistics for compressed scoring
- **AND** it SHALL NOT require embedded payload content to produce compressed candidates

### Requirement: The raw vector plane SHALL support fixed-cost row-id addressing
The baseline SHALL support exact rerank by loading raw vectors directly from disk using row id addressing.

#### Scenario: Raw vectors use row-major fixed-length addressing
- **WHEN** a rerank step needs the raw vector of one candidate record
- **THEN** the baseline SHALL be able to derive that record's byte position from its `row_id`
- **AND** the addressing logic SHALL NOT depend on decoding unrelated cluster blocks

#### Scenario: Raw vector reads remain decoupled from compressed cluster ordering
- **WHEN** coarse cluster assignment changes during a rebuild
- **THEN** the row-id addressing scheme for the raw vector plane SHALL remain stable for the same exported dataset ordering
- **AND** rerank correctness SHALL NOT depend on cluster-local raw vector placement
