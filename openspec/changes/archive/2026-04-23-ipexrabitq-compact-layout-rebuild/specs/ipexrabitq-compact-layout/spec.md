## ADDED Requirements

### Requirement: Stage2 compact layout SHALL define a rebuild-required `v11` blocked format
The system SHALL define a new rebuild-required `v11` Stage2 storage format for ExRaBitQ data, rather than reusing the existing `v10 packed_sign` per-vector layout.

#### Scenario: `v11` is treated as a distinct Stage2 storage version
- **WHEN** a Stage2-optimized compact index is built
- **THEN** the resulting cluster store SHALL record a distinct `v11` Stage2 storage version
- **AND** the query path SHALL be able to distinguish it from `v10 packed_sign`

### Requirement: `v11` SHALL store Stage2 data in batch-major blocked form
The `v11` Stage2 region SHALL store ExRaBitQ payloads in a batch-major blocked layout with `batch_size=8` and `dim_block=64`, rather than storing each candidate's full Stage2 payload contiguously.

#### Scenario: One Stage2 batch block contains eight lanes and 64-dim sub-blocks
- **WHEN** the builder serializes one `v11` Stage2 batch block
- **THEN** it SHALL encode the block as eight candidate lanes grouped by 64-dimension sub-blocks
- **AND** it SHALL not serialize the block as eight independent `code_abs + sign + xipnorm` records laid out back-to-back

### Requirement: `v11` SHALL preserve Stage2 numeric payload types
The `v11` compact layout SHALL continue to store Stage2 magnitudes as `uint8`, signs as packed bits, and `xipnorm` as float values.

#### Scenario: Compact layout retains existing Stage2 numeric domains
- **WHEN** a `v11` Stage2 block is materialized
- **THEN** its abs payload SHALL remain `uint8`
- **AND** its sign payload SHALL remain packed-bit encoded
- **AND** its `xipnorm` payload SHALL remain float-based

### Requirement: `v11` SHALL encode valid lane count for tail blocks
The `v11` Stage2 layout SHALL encode how many lanes are valid in the final batch block, so that the query path can avoid treating padded lanes as real candidates.

#### Scenario: Tail block records valid lane count
- **WHEN** the final Stage2 batch block contains fewer than 8 candidates
- **THEN** the serialized block SHALL preserve the real valid lane count
- **AND** padded lanes SHALL not change query-serving semantics

### Requirement: Compact Stage2 serving SHALL not depend on online repacking
The compact Stage2 serving path SHALL consume the `v11` blocked format directly and SHALL NOT require rebuilding a batch-friendly layout online during query execution.

#### Scenario: Query path reads compact blocks directly
- **WHEN** Stage2 serving uses a `v11` compact index
- **THEN** the query path SHALL read the persisted blocked Stage2 layout directly
- **AND** it SHALL not first repack `v10`-style per-vector payloads into a temporary batch layout on the hot path
