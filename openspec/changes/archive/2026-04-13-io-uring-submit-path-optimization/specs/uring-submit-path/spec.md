## ADDED Requirements

### Requirement: Query path SHALL support configurable io_uring submission configuration
The query path SHALL allow `io_uring` submission behavior to be configured at runtime, including queue depth and whether SQPOLL is enabled. The effective configuration SHALL be applied to the reader initialization used by the search scheduler rather than remaining a benchmark-only placeholder.

#### Scenario: Queue depth affects reader initialization
- **WHEN** a search run is configured with `io_queue_depth = 256`
- **THEN** the `IoUringReader` used by the scheduler SHALL be initialized with queue depth 256
- **AND** the search run SHALL not silently fall back to the previous hard-coded depth of 64

#### Scenario: SQPOLL is explicitly selectable
- **WHEN** a search run enables SQPOLL mode
- **THEN** the `IoUringReader` SHALL attempt to initialize the ring with SQPOLL enabled
- **AND** the effective mode SHALL be observable to the caller through benchmark output or status reporting

### Requirement: Scheduler SHALL minimize submit-side syscall frequency
The scheduler SHALL reduce submit-side syscall frequency by coalescing prepared cluster reads and vec/payload reads into fewer submit points. Before any blocking wait on completions, the scheduler SHALL flush all prepared SQEs required for forward progress.

#### Scenario: Blocking wait performs a safety flush
- **WHEN** the scheduler is about to block waiting for a cluster completion
- **AND** there are prepared but not yet submitted SQEs needed to make progress
- **THEN** the scheduler SHALL submit those SQEs before entering the blocking wait

#### Scenario: Same-iteration cluster refill and vec reads avoid duplicate submit points
- **WHEN** a scheduling iteration produces both vec/payload SQEs and cluster refill SQEs
- **THEN** the implementation SHALL support coalescing them into fewer submit calls than the previous per-path flush behavior
- **AND** the search SHALL preserve the same completion semantics and result ordering

### Requirement: Scheduler SHALL support isolating cluster I/O from data I/O
The scheduler SHALL support a submission mode that prevents cluster block reads from starving or prematurely flushing vec/payload batching. This MAY be implemented by shared-ring capacity isolation or by separate rings, but the selected mode MUST preserve strict cluster probe order and correct completion dispatch.

#### Scenario: Shared submission mode preserves ordering
- **WHEN** cluster block reads and vec/payload reads use a shared submission path
- **THEN** cluster probing SHALL still follow nearest-cluster order
- **AND** completions from different I/O types SHALL be dispatched to the correct handling path

#### Scenario: Isolated submission mode preserves behavior
- **WHEN** cluster block reads and vec/payload reads are configured to run in isolated submission mode
- **THEN** the search SHALL return the same result set and ordering as shared mode for the same query and index
- **AND** completion processing SHALL continue to support mixed cluster, vec, and payload events without deadlock

### Requirement: Completion bookkeeping SHALL scale with deeper queues
The completion bookkeeping used by the scheduler SHALL support deeper queue depths without relying solely on pointer-keyed hash lookup on the hot path. The system SHALL preserve correct buffer ownership and cleanup across normal completion, early stop, and query teardown.

#### Scenario: Completion dispatch identifies pending I/O without buffer hash lookup
- **WHEN** a completion is received for an in-flight request under the optimized bookkeeping path
- **THEN** the scheduler SHALL recover the corresponding pending I/O metadata directly from completion-associated state
- **AND** it SHALL dispatch the completion to the same logical handler as before

#### Scenario: Early stop cleans up optimized bookkeeping state
- **WHEN** a query exits early while there are pending or in-flight reads tracked by the optimized bookkeeping path
- **THEN** the scheduler SHALL flush or drain the remaining reads as required for correctness
- **AND** any remaining bookkeeping state SHALL be safely released during query teardown
