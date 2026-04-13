## ADDED Requirements

### Requirement: Warm-Only Primary Evaluation Protocol
The project SHALL define warm steady-state as the only required primary evaluation protocol for the post-baseline BoundFetch experiment phase.

#### Scenario: Warm-only protocol is declared as primary
- **WHEN** the experiment proposal, plan, or tracker is updated for the next phase
- **THEN** the active protocol SHALL be described as warm steady-state
- **AND** cold-start, drop-cache, or sudo-dependent evaluation SHALL NOT be listed as required main-path experiments

#### Scenario: Non-goal is recorded explicitly
- **WHEN** the updated experiment artifacts are written
- **THEN** they SHALL state that cold-start is a non-goal for the current paper story
- **AND** they SHALL explain that the deployment target is production-style steady-state serving

### Requirement: Warm E2E Pareto Is the Main Anchor Result
The project SHALL treat the warm end-to-end recall-latency Pareto on `coco_100k` as the main anchor result for the next experiment stage.

#### Scenario: Main result includes BoundFetch operating points
- **WHEN** the primary experiment block is executed
- **THEN** it SHALL include multiple BoundFetch operating points rather than a single selected point
- **AND** the output SHALL include `recall@10` and `e2e_ms` for each point

#### Scenario: Main result compares against separated baselines
- **WHEN** the warm anchor result is reported
- **THEN** it SHALL include the strongest available separated baseline points
- **AND** the comparison SHALL be framed as end-to-end `search + payload` evaluation

### Requirement: Optimization Gate Follows Recall Improvement
The project SHALL decide whether additional code optimization is worthwhile based on recall improvement at near-current latency, not on further queue-depth or submission micro-tuning alone.

#### Scenario: Further optimization is evaluated
- **WHEN** a new optimization direction is proposed after the baseline stage
- **THEN** it SHALL be justified by expected movement in the recall-latency tradeoff
- **AND** it SHALL NOT be justified only by lower-level tuning interest

#### Scenario: Recall-improvement ablation is prioritized
- **WHEN** the next implementation-ready experiment batch is defined
- **THEN** a minimal ablation over threshold or policy parameters SHALL appear before any new broad systems optimization batch
