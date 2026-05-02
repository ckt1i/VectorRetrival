## ADDED Requirements

### Requirement: Stage2 scheduling SHALL operate on `block_id + lane_mask`
When serving a `v11` compact index, Stage2 scheduling SHALL map uncertain candidates to `batch_block_id` and `lane_id`, rather than representing Stage2 work only as a list of independent candidate pointers.

#### Scenario: Uncertain candidates are grouped by Stage2 block
- **WHEN** Stage1 emits uncertain candidates for a `v11` compact cluster
- **THEN** the query path SHALL group them by `batch_block_id`
- **AND** it SHALL derive lane membership within each block from `lane_id`

### Requirement: Stage2 kernel dispatch SHALL consume batch-block views
The `v11` Stage2 serving path SHALL dispatch the Stage2 kernel using a batch-block view plus a lane-selection signal, rather than passing eight unrelated `code_abs/sign` pointers as the primary hot-path representation.

#### Scenario: Stage2 kernel is called with a block view
- **WHEN** Stage2 boosting runs on a `v11` compact block
- **THEN** the query path SHALL call the Stage2 kernel with a batch-block-aligned Stage2 view
- **AND** it SHALL limit work to the lanes selected for that block

### Requirement: Block-aware Stage2 scheduling SHALL preserve funnel semantics
Moving from candidate-list Stage2 scheduling to block-aware scheduling SHALL NOT change Stage1 SafeOut / SafeIn / Uncertain semantics, final top-k semantics, or resident serving semantics.

#### Scenario: Block-aware scheduling preserves query results
- **WHEN** the same query is run against semantically equivalent `v10` and `v11` indexes under the same serving settings
- **THEN** Stage2 block-aware scheduling SHALL preserve the same recall and ranking semantics
- **AND** resident serving behavior SHALL remain compatible

### Requirement: Block-aware Stage2 scheduling SHALL ignore padded lanes
If a `v11` batch block contains padded lanes in the final block, Stage2 scheduling SHALL ensure those lanes are never treated as real candidates.

#### Scenario: Tail-block padding does not create fake candidates
- **WHEN** Stage2 evaluates the final compact batch block and that block contains padded lanes
- **THEN** the scheduler SHALL exclude padded lanes from Stage2 boosting and classification
- **AND** those lanes SHALL not appear in candidate, rerank, or benchmark statistics
