## ADDED Requirements

### Requirement: Thesis minimal main execution SHALL cover only the `T000-T015` thesis main-sweep matrix
The thesis minimal main-sweep contract SHALL cover only the asset checks, canonical index preparation, reuse validation, sanity runs, and full sweeps needed for `coco_100k` and `msmarco_passage` under `T000-T015`.

#### Scenario: The primary thesis matrix is frozen before execution
- **WHEN** the thesis minimal main-sweep change is used to schedule execution
- **THEN** the active datasets SHALL be only `coco_100k` and `msmarco_passage`
- **AND** the active systems SHALL be `BoundFetch-Guarded`, `IVF+PQ+FlatStor`, `IVF+RaBitQ+FlatStor`, `IVF+PQ+Lance`, and `IVF+RaBitQ+Lance`
- **AND** `deep1m_synth`, `deep8m_synth`, Amazon ESCI, appendix datasets, and non-thesis baseline families SHALL remain out of scope for this change

#### Scenario: The query and top-k protocol is frozen
- **WHEN** a run is scheduled under this change
- **THEN** the run SHALL use `queries=1000`
- **AND** the main-sweep protocol SHALL use `topk=10`
- **AND** baseline rerank runs SHALL use `candidate_budget=100`

### Requirement: Thesis main execution SHALL enforce an ordered gate sequence before full sweeps
The thesis main-sweep SHALL execute through a fixed gate sequence so that canonical index artifacts are prepared and validated before any full experiment sweep begins.

#### Scenario: COCO full sweeps wait for asset and index gates
- **WHEN** `coco_100k` is scheduled for thesis main execution
- **THEN** `T000` asset validation SHALL complete before any index build starts
- **AND** the canonical `coco_100k` index build SHALL complete before `T002-T009`
- **AND** reuse validation for BoundFetch-Guarded, `IVF+PQ`, and `IVF+RaBitQ` SHALL pass before `T005-T009` full sweeps may start

#### Scenario: MS MARCO full sweeps wait for asset and index gates
- **WHEN** `msmarco_passage` is scheduled for thesis main execution
- **THEN** `T001` asset validation SHALL complete before any index build starts
- **AND** the canonical `msmarco_passage` index build SHALL complete before `T010-T015`
- **AND** reuse validation for BoundFetch-Guarded, `IVF+PQ`, and `IVF+RaBitQ` SHALL pass before `T011-T015` full sweeps may start

### Requirement: Thesis main execution SHALL use fixed dataset-specific operating-point sweeps
The thesis main-sweep SHALL freeze the dataset-level operating-point grids so that comparison remains aligned with the thesis experiment plan and reduced formal-study controls.

#### Scenario: COCO operating-point sweep is fixed
- **WHEN** `coco_100k` main-sweep runs are scheduled
- **THEN** the run SHALL use `nlist=2048`
- **AND** the sweep axis SHALL be `nprobe ∈ {16, 32, 64, 128, 256, 512}`
- **AND** `T002-T004` sanity runs SHALL use a single-point operating point within that same contract

#### Scenario: MS MARCO operating-point sweep is fixed
- **WHEN** `msmarco_passage` main-sweep runs are scheduled
- **THEN** the run SHALL use `nlist=16384`
- **AND** the sweep axis SHALL be `nprobe ∈ {16, 32, 64, 128, 256, 512, 1024}`
- **AND** `T010` sanity SHALL be a single-point run within that same contract

### Requirement: Thesis main execution SHALL use fixed method parameters for BoundFetch-Guarded
The thesis main-sweep SHALL freeze the BoundFetch-Guarded mainline parameters so that the method is measured under the intended candidate-filtering semantics.

#### Scenario: BoundFetch mainline parameters are fixed
- **WHEN** BoundFetch-Guarded is scheduled for any thesis main sanity run or full sweep
- **THEN** the run SHALL use `crc=1`
- **AND** the run SHALL use `early-stop=0`
- **AND** the run SHALL use `bits=4`
- **AND** the run SHALL use strict recall for measurement with `skip_gt=0`

#### Scenario: Non-mainline BoundFetch parameters are not thesis mainline results
- **WHEN** a BoundFetch run uses parameters outside the frozen thesis mainline settings
- **THEN** the run SHALL NOT be mixed into the thesis main conclusion
- **AND** the tracker SHALL distinguish it from the `crc=1, early-stop=0` mainline
- **AND** any such result SHALL be interpreted as a separate debug, tuning, or ablation mode rather than a replacement for BoundFetch-Guarded mainline execution

### Requirement: Thesis main execution SHALL separate warmup from formal measurement
Each thesis main-sweep operating point SHALL run through a warmup step before the formal measurement step so that cold-read and initialization effects do not pollute the thesis latency metrics.

#### Scenario: Warmup precedes every full-sweep measurement
- **WHEN** any full-sweep operating point is scheduled for `T005-T009` or `T011-T015`
- **THEN** the workflow SHALL first run a warmup invocation for the same dataset, system, canonical artifact, nprobe, bits, storage backend, `crc`, and `early-stop` settings
- **AND** the warmup output SHALL NOT be used as the thesis main recall/latency result
- **AND** the formal measurement invocation SHALL start only after the warmup invocation has completed

#### Scenario: Formal measurement uses strict recall
- **WHEN** a full-sweep measurement invocation is recorded as a thesis main result
- **THEN** it SHALL use `skip_gt=0`
- **AND** it SHALL load the approved GT file for the dataset when recall is available
- **AND** it SHALL export recall and latency fields from the measurement run rather than from the warmup run

### Requirement: Thesis main execution SHALL preserve the thesis sanity gates before dataset-wide sweeps
The thesis main-sweep SHALL keep explicit sanity gates so that schema, metrics, and runner compatibility issues are caught before spending time on the full matrix.

#### Scenario: COCO sanity completes before the COCO full matrix
- **WHEN** `coco_100k` enters the execution phase
- **THEN** the workflow SHALL first run `T002` for BoundFetch-Guarded
- **AND** it SHALL then run `T003` for `IVF+PQ+FlatStor`
- **AND** it SHALL then run `T004` for `IVF+RaBitQ+FlatStor`
- **AND** the COCO full sweeps SHALL remain blocked until these sanity runs export the required recall and latency fields

#### Scenario: MS MARCO sanity completes before the MS MARCO full matrix
- **WHEN** `msmarco_passage` enters the execution phase
- **THEN** the workflow SHALL first run `T010` for BoundFetch-Guarded
- **AND** the MS MARCO full sweeps SHALL remain blocked until that sanity run exports the required recall and latency fields
