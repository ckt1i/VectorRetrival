## ADDED Requirements

### Requirement: Query pipeline SHALL expose a compact Stage2 batch-block view
The query pipeline SHALL expose a batch-block Stage2 view for compact `v11` indexes, rather than requiring the Stage2 hot path to reconstruct a batch from independent per-candidate pointers.

#### Scenario: Parsed cluster exposes compact Stage2 block view
- **WHEN** a resident or parsed cluster is opened from a `v11` compact index
- **THEN** the query pipeline SHALL be able to retrieve a Stage2 batch-block view from that cluster
- **AND** that view SHALL describe a compact Stage2 block rather than a single candidate payload

### Requirement: Query pipeline SHALL support compact Stage2 block-aware dispatch
The query pipeline SHALL support dispatching Stage2 boosting by compact batch block and lane selection, rather than only by uncertain candidate list iteration.

#### Scenario: Stage2 dispatch follows block order
- **WHEN** Stage1 produces uncertain candidates for a `v11` compact cluster
- **THEN** the query pipeline SHALL transform them into block-aware Stage2 dispatch units
- **AND** it SHALL drive Stage2 boosting from those block-aware units

### Requirement: Compact Stage2 integration SHALL preserve query semantics
Introducing compact Stage2 block views and block-aware dispatch SHALL NOT change recall computation, final result ordering, CRC semantics, payload semantics, or resident serving semantics.

#### Scenario: Compact Stage2 path preserves serving contract
- **WHEN** the query pipeline serves a query from a compact `v11` index
- **THEN** the final query contract SHALL remain compatible with the existing serving path
- **AND** compact Stage2 integration SHALL not require changes to the user-visible search interface
