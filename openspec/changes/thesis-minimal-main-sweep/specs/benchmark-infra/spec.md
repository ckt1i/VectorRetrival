## ADDED Requirements

### Requirement: Benchmark infrastructure SHALL publish canonical thesis index-build artifacts
The benchmark infrastructure SHALL support a stable artifact-publishing workflow for thesis minimal main-sweep index builds so downstream methods can reuse the same dataset-level artifacts.

#### Scenario: Canonical artifact publication is stable for COCO and MS MARCO
- **WHEN** the thesis workflow publishes a canonical artifact for `coco_100k` or `msmarco_passage`
- **THEN** the artifact SHALL be written to a stable dataset-specific directory
- **AND** the publication metadata SHALL record dataset identity, builder identity, `nlist`, metric, and source run id

#### Scenario: Artifact publication distinguishes builder choice
- **WHEN** canonical artifacts are generated for different thesis datasets
- **THEN** the publication metadata SHALL distinguish `faiss_kmeans` from `hierarchical_superkmeans`
- **AND** downstream consumers SHALL be able to resolve which builder produced each artifact without inferring it from filenames alone

### Requirement: Benchmark infrastructure SHALL support downstream reuse validation against published artifacts
The benchmark infrastructure SHALL provide enough structured metadata for downstream method runners to validate reuse of a published canonical artifact.

#### Scenario: Downstream runner can resolve a published artifact
- **WHEN** a thesis method starts a reuse-validation or full-sweep run
- **THEN** it SHALL be able to resolve the intended canonical artifact from published metadata
- **AND** it SHALL not require manual renaming or ad hoc directory guessing to locate the artifact

#### Scenario: Reuse validation results stay attributable to the published artifact
- **WHEN** a downstream method completes a reuse-validation run against a published artifact
- **THEN** the recorded result SHALL preserve the published artifact identity
- **AND** the infrastructure SHALL support later aggregation of reuse-validation outcomes by dataset and builder
