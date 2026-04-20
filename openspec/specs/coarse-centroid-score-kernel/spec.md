## ADDED Requirements

### Requirement: Coarse centroid score SHALL use a SIMD register-blocking kernel on the `effective_metric=ip` path
The coarse cluster selection path SHALL replace the current scalar centroid-by-centroid dot-product loop with a SIMD kernel that evaluates multiple centroids in parallel for the `effective_metric=ip` path.

#### Scenario: Multiple centroids are scored within one kernel block
- **WHEN** `FindNearestClusters()` computes coarse scores for an `effective_metric=ip` index
- **THEN** the implementation SHALL process a block of centroids in one kernel invocation rather than one centroid at a time
- **AND** the query vector block SHALL be reused across the centroids in that block

#### Scenario: Scalar fallback remains available
- **WHEN** the binary is built without the target SIMD ISA
- **THEN** the coarse score path SHALL fall back to a scalar implementation
- **AND** the query semantics and top-n output contract SHALL remain unchanged

### Requirement: Coarse centroid score SHALL consume a packed centroid layout prepared before query execution
The SIMD coarse score path SHALL read from a packed centroid layout that is prepared before query execution, rather than packing centroids during the query.

#### Scenario: Packed layout is ready before the first query
- **WHEN** an IVF index is opened for query serving
- **THEN** the coarse score path SHALL have access to a packed centroid layout derived from `centroids_`
- **AND** if normalized centroids exist for cosine serving, it SHALL also have a packed normalized-centroid layout

#### Scenario: Query path does not repack centroids
- **WHEN** a query enters `FindNearestClusters()`
- **THEN** the query path SHALL directly consume the prepared packed centroid layout
- **AND** it SHALL NOT build or repack centroid blocks on the fly for the current query

### Requirement: Packed centroid layout SHALL define deterministic tail handling
The packed centroid layout SHALL define how centroid-block tails and dimension tails are handled so that SIMD and scalar paths remain comparable.

#### Scenario: Tail centroid block is padded or masked deterministically
- **WHEN** `nlist` is not an exact multiple of the centroid block size
- **THEN** the final centroid block SHALL use a deterministic padding or masking rule
- **AND** inactive lanes SHALL NOT affect the final score ordering

#### Scenario: Tail dimension block is padded or masked deterministically
- **WHEN** `dim` is not an exact multiple of the SIMD vector width
- **THEN** the final dimension block SHALL use a deterministic padding or masking rule
- **AND** the resulting coarse scores SHALL remain comparable with the scalar path
