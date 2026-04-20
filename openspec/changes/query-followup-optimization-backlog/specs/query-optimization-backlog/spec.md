## ADDED Requirements

### Requirement: Query optimization backlog SHALL record prioritized follow-up hotspots
The system documentation SHALL maintain a single backlog capability for query-path follow-up optimizations, and that backlog SHALL record each remaining hotspot with an explicit priority, rationale, and estimated latency-saving range.

#### Scenario: Remaining hotspots are recorded with priority and estimate
- **WHEN** a follow-up query optimization point is added to the backlog
- **THEN** the record SHALL include the hotspot name, current hotspot rationale, recommended priority, and an estimated `avg_query_ms` saving range

### Requirement: Query optimization backlog SHALL preserve the profiling gate for each item
Each backlog item SHALL record the measurement basis that justifies doing the work next, including whether the item is driven by query-only perf, full E2E timing, or both.

#### Scenario: Backlog item records its profiling basis
- **WHEN** a backlog item is documented
- **THEN** the item SHALL state whether its priority is justified by query-only perf hotspots, full E2E latency contribution, or both

### Requirement: Query optimization backlog SHALL define an execution order
The backlog SHALL define a recommended execution order so that later work does not preempt earlier higher-ROI work without an explicit reason.

#### Scenario: Recommended optimization sequence is visible
- **WHEN** the backlog is reviewed for the next optimization round
- **THEN** it SHALL expose a recommended execution order for the recorded items
- **AND** that order SHALL make it possible to choose the next item without reconstructing the earlier profiling discussion

### Requirement: Query optimization backlog SHALL record the first-round `probe_submit` work packages explicitly
The backlog SHALL explicitly record the first-round `probe_submit` work packages as concrete implementation-ready backlog items rather than leaving them as generic hotspot notes.

#### Scenario: First-round `probe_submit` work packages are visible
- **WHEN** the backlog is reviewed for implementation planning
- **THEN** it SHALL explicitly include `batched submit skeleton`
- **AND** it SHALL explicitly include `batch-aware dedup`
- **AND** it SHALL explicitly include `CRC/timing off hotpath`

### Requirement: First-round `probe_submit` work SHALL preserve query-result semantics
The first-round `probe_submit` work packages SHALL preserve the existing query-result semantics while changing only the submission-path organization.

#### Scenario: Submit-path refactor keeps search semantics unchanged
- **WHEN** a first-round `probe_submit` work package is implemented
- **THEN** it SHALL preserve SafeIn / Uncertain classification semantics
- **AND** it SHALL preserve rerank and payload semantics
- **AND** it SHALL preserve recall and final result ordering semantics

## MODIFIED Requirements

### Requirement: Query pipeline optimization work SHALL preserve the existing phase-based attribution boundary
The query pipeline SHALL preserve the existing phase-based attribution boundary when follow-up optimizations are analyzed or implemented, so that later changes remain comparable with the current breakdown.

#### Scenario: Follow-up optimization remains attributable
- **WHEN** a later optimization changes the query path
- **THEN** the analysis and validation SHALL remain attributable against the existing phase breakdown
- **AND** the optimization SHALL identify which phase or sub-phase it is expected to reduce

### Requirement: E2E optimization validation SHALL use a stable dual-benchmark protocol
Any follow-up query optimization SHALL be validated with both a full E2E run and a query-only perf run under the same operating point, so that CPU hotspot movement and end-to-end impact remain jointly observable.

#### Scenario: Optimization round is measured with both benchmark modes
- **WHEN** a follow-up optimization item is evaluated
- **THEN** the evaluation SHALL include a full E2E measurement under the fixed operating point
- **AND** it SHALL include a query-only perf measurement under the same query parameters

#### Scenario: First-round `probe_submit` work records estimated benefit ranges
- **WHEN** the backlog documents the first-round `probe_submit` work packages
- **THEN** each package SHALL include an estimated `avg_query_ms` improvement range
- **AND** those estimates SHALL be recorded as ranges rather than single fixed values
