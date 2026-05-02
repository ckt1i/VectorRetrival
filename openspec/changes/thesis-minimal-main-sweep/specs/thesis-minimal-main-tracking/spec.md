## ADDED Requirements

### Requirement: Thesis main tracking SHALL represent `T000-T015` as subtask-addressable execution work
The thesis main-sweep tracker SHALL preserve enough detail to distinguish asset checks, index preparation, reuse validation, sanity runs, and full sweeps within `T000-T015`.

#### Scenario: Tracker rows distinguish gate type and parent task
- **WHEN** a thesis main-sweep run or gate is recorded
- **THEN** the tracker SHALL record whether it is an asset check, canonical index build, reuse validation, sanity run, full sweep, or summary step
- **AND** the record SHALL remain attributable to its parent thesis task id within `T000-T015`

#### Scenario: Subtasks remain individually auditable
- **WHEN** one step inside a thesis task fails or is retried
- **THEN** the failed or superseded subtask SHALL remain visible in tracker history
- **AND** a later retry SHALL receive its own distinct run record rather than overwriting the earlier attempt

### Requirement: Thesis main tracking SHALL use canonical output roots for new artifact classes
The thesis main-sweep SHALL extend the formal-study output layout with explicit roots for canonical index build and reuse validation, while preserving the existing vector-search and E2E roots.

#### Scenario: Canonical index build outputs use a stable root
- **WHEN** a thesis dataset finishes its canonical index build
- **THEN** its build metadata and provenance SHALL be written under `outputs/index_build/{dataset}/{builder}/{run_id}/`
- **AND** the output SHALL be sufficient to recover the canonical artifact directory and builder identity later

#### Scenario: Reuse validation outputs use a stable root
- **WHEN** a thesis method completes a canonical-artifact reuse validation run
- **THEN** the validation metadata SHALL be written under `outputs/index_reuse_validation/{dataset}/{system}/{run_id}/`
- **AND** the output SHALL make the consumed canonical artifact reconstructable

### Requirement: Thesis main tracking SHALL emit dataset-level thesis summaries after each full sweep block
The thesis main-sweep SHALL generate dataset-level thesis summaries as soon as the required full-sweep block for that dataset is complete.

#### Scenario: COCO thesis summary is emitted after `T005-T009`
- **WHEN** the required COCO full sweeps have completed or been explicitly marked blocked with rationale
- **THEN** the workflow SHALL emit a COCO dataset summary capturing matched-quality selection inputs and full-sweep outputs
- **AND** the summary SHALL remain separate from later MS MARCO aggregation

#### Scenario: MS MARCO thesis summary is emitted after `T011-T015`
- **WHEN** the required MS MARCO full sweeps have completed or been explicitly marked blocked with rationale
- **THEN** the workflow SHALL emit an MS MARCO dataset summary capturing matched-quality selection inputs and full-sweep outputs
- **AND** delayed qrels-derived metrics SHALL be marked explicitly rather than silently omitted
