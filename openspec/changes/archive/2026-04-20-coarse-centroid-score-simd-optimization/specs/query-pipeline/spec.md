## MODIFIED Requirements

### Requirement: OverlapScheduler / query path SHALL allow coarse selection to use prebuilt packed centroid layouts
The query pipeline SHALL allow coarse cluster selection to consume a centroid layout prepared before query execution, so that the coarse score kernel can focus on computation rather than layout conversion.

#### Scenario: Packed coarse layout is used transparently during query
- **WHEN** `FindNearestClusters()` runs on an index that prepared a coarse packed layout during `Open()`
- **THEN** the query path SHALL use that packed layout for coarse scoring
- **AND** the rest of the query pipeline SHALL continue to receive the same ordered cluster IDs as before

#### Scenario: Query normalize and top-n selection remain semantically unchanged
- **WHEN** the coarse centroid score kernel is optimized
- **THEN** the query pipeline SHALL keep the existing query normalize behavior for cosine serving
- **AND** it SHALL keep the existing top-n selection semantics after score computation
