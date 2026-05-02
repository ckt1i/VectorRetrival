## ADDED Requirements

### Requirement: Stage2 SHALL support batch candidate boosting as a separate execution mode
The Stage2 boosting path SHALL support a batch candidate execution mode that processes a group of uncertain candidates together, instead of requiring each uncertain candidate to be boosted immediately when visited.

#### Scenario: Uncertain candidates are boosted as a batch
- **WHEN** Stage2 batch boosting mode is enabled
- **THEN** the system SHALL allow multiple uncertain candidates from the current query path to be collected and boosted together
- **AND** the final ranking semantics SHALL remain unchanged

### Requirement: Batch boosting SHALL preserve Stage1 funnel semantics
The Stage2 batch boosting mode SHALL keep the existing Stage1 candidate funnel semantics, including SafeOut filtering and the meaning of SafeIn and Uncertain candidates.

#### Scenario: Stage1 semantics remain unchanged
- **WHEN** batch boosting is introduced after Stage1 filtering
- **THEN** Stage1 SafeOut candidates SHALL remain excluded from Stage2 boosting
- **AND** candidates that reach Stage2 SHALL follow the same Stage1 funnel semantics as before

### Requirement: Batch boosting SHALL reuse query-side state across candidates
The batch boosting mode SHALL be allowed to reuse query-side prepared state and Stage2 scratch across multiple candidates in the same batch, rather than re-entering the full Stage2 kernel path independently for each candidate.

#### Scenario: Query-side state is shared within a batch
- **WHEN** a batch of uncertain candidates is processed by Stage2
- **THEN** the batch boosting implementation SHALL be allowed to reuse query-side prepared state across the candidates in that batch

### Requirement: First-stage batch boosting SHALL not require a persistent format change
The first implementation of Stage2 batch boosting SHALL work with the existing ExRaBitQ storage format and SHALL NOT require a new persisted ex-data layout as a prerequisite.

#### Scenario: Batch boosting runs on current ex-data layout
- **WHEN** the first batch boosting implementation is enabled
- **THEN** it SHALL be able to consume the current Stage2 ex-data representation without requiring a rebuild-only format migration

### Requirement: Stage2 batch boosting SHALL be gated by post-kernel perf results
The batch boosting route SHALL be treated as the second optimization stage and SHALL be evaluated only after the dedicated packed-sign kernel route has been measured.

#### Scenario: Batch boosting is entered after kernel evaluation
- **WHEN** the dedicated packed-sign kernel has already been benchmarked and profiled
- **THEN** batch boosting SHALL be considered the next optimization stage only if Stage2 remains a major hotspot
