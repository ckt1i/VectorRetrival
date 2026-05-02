## ADDED Requirements

### Requirement: Query pipeline SHALL support a packed-sign dedicated Stage2 kernel path
The query pipeline SHALL support a Stage2 execution path that dispatches candidates with v10 packed-sign ExRaBitQ payloads to a packed-sign dedicated `IPExRaBitQ` kernel.

#### Scenario: Packed-sign candidate dispatches to dedicated Stage2 path
- **WHEN** the query pipeline boosts a candidate whose Stage2 payload is v10 packed-sign
- **THEN** the pipeline SHALL dispatch that candidate through the packed-sign dedicated Stage2 kernel path

### Requirement: Query pipeline SHALL support a batch Stage2 boosting path
The query pipeline SHALL support a Stage2 batch boosting path that collects uncertain candidates and boosts them as a batch while preserving the same top-k result semantics.

#### Scenario: Stage2 boosting happens after candidate collection
- **WHEN** batch Stage2 boosting mode is active
- **THEN** the query pipeline SHALL be able to collect uncertain candidates before issuing Stage2 boosting work
- **AND** the final top-k semantics SHALL match the non-batch path

### Requirement: Query pipeline SHALL preserve serving semantics across both Stage2 routes
The query pipeline SHALL preserve recall, ranking, CRC, payload, and resident-serving semantics regardless of whether Stage2 uses the packed-sign dedicated kernel route or the batch boosting route.

#### Scenario: Stage2 route does not change serving semantics
- **WHEN** the query pipeline is run with the same query, index, and serving parameters under different Stage2 optimization routes
- **THEN** the serving result semantics SHALL remain unchanged
