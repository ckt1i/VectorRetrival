## ADDED Requirements

### Requirement: Thesis main-sweep SHALL build canonical dataset artifacts with `bench_e2e` before method execution
The thesis main-sweep SHALL treat `bench_e2e` as the canonical producer of dataset-level clustering and index artifacts used by later method execution.

#### Scenario: COCO canonical artifact uses `faiss_kmeans`
- **WHEN** `coco_100k` enters the canonical index build phase after `T000`
- **THEN** the workflow SHALL build the canonical artifact with `bench_e2e --coarse-builder faiss_kmeans`
- **AND** the build output SHALL record dataset identity, builder identity, `nlist=2048`, metric, and the artifact directory used for later reuse

#### Scenario: MS MARCO canonical artifact uses `hierarchical_superkmeans`
- **WHEN** `msmarco_passage` enters the canonical index build phase after `T001`
- **THEN** the workflow SHALL build the canonical artifact with `bench_e2e --coarse-builder hierarchical_superkmeans`
- **AND** the build output SHALL record dataset identity, builder identity, `nlist=16384`, metric, and the artifact directory used for later reuse

### Requirement: Thesis methods SHALL consume canonical artifacts without raw-vector retraining
After a canonical artifact is published for a thesis dataset, BoundFetch-Guarded and the baseline methods SHALL consume that artifact directly or through a deterministic import path that does not retrain or re-cluster on the raw base vectors.

#### Scenario: BoundFetch reuses a canonical artifact
- **WHEN** BoundFetch-Guarded runs on a thesis dataset with an approved canonical artifact
- **THEN** it SHALL accept the artifact through a stable external index input such as `--index-dir`
- **AND** the run SHALL record the canonical artifact provenance rather than silently rebuilding from the raw vectors

#### Scenario: Baseline methods reuse or import the canonical artifact without retraining
- **WHEN** `IVF+PQ` or `IVF+RaBitQ` runs on a thesis dataset with an approved canonical artifact
- **THEN** the method SHALL either directly consume the canonical artifact or execute a deterministic import step derived only from that artifact
- **AND** the method SHALL NOT recompute coarse clustering, centroid training, or assignment generation from the raw vectors once the canonical artifact exists

### Requirement: Canonical-artifact reuse SHALL be validated before full sweeps
The thesis main-sweep SHALL require an explicit reuse-validation gate before any dataset-wide full sweep starts.

#### Scenario: COCO reuse validation precedes `T005-T009`
- **WHEN** `coco_100k` canonical artifacts have been built
- **THEN** the workflow SHALL run a validation step showing BoundFetch-Guarded, `IVF+PQ`, and `IVF+RaBitQ` can execute from the canonical artifact path
- **AND** a failure in any of these reuse validations SHALL block `T005-T009` until the failure is resolved or explicitly downgraded

#### Scenario: MS MARCO reuse validation precedes `T011-T015`
- **WHEN** `msmarco_passage` canonical artifacts have been built
- **THEN** the workflow SHALL run a validation step showing BoundFetch-Guarded, `IVF+PQ`, and `IVF+RaBitQ` can execute from the canonical artifact path
- **AND** a failure in any of these reuse validations SHALL block `T011-T015` until the failure is resolved or explicitly downgraded
