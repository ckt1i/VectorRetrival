## ADDED Requirements

### Requirement: The IVF-RaBitQ baseline SHALL build its coarse partitioning with Faiss-compatible semantics
The baseline build path SHALL use Faiss or Faiss-equivalent clustering logic for `nlist` training and vector-to-cluster assignment.

#### Scenario: Build CLI accepts Faiss-style coarse parameters
- **WHEN** a user launches the baseline build command
- **THEN** the command SHALL accept at least `--nlist`, `--metric`, `--train-size`, and `--output`
- **AND** those parameters SHALL determine the coarse quantizer training and output layout

#### Scenario: Coarse centroids are persisted for later probing
- **WHEN** coarse training completes
- **THEN** the resulting coarse centroids SHALL be written to disk in the compressed index plane
- **AND** a later search run SHALL be able to reuse them without retraining

### Requirement: The build path SHALL encode cluster records with the official RaBitQ library
The baseline build path SHALL rely on the official `RaBitQ-Library` for compressed code generation rather than reusing BoundFetch-specific query logic.

#### Scenario: Cluster-local RaBitQ codes are produced during build
- **WHEN** vectors are assigned to their coarse clusters
- **THEN** the build pipeline SHALL encode the assigned records into RaBitQ-compatible compressed representations
- **AND** the resulting codes SHALL be persisted into `clusters.clu`

#### Scenario: Build metadata records the effective encoding configuration
- **WHEN** a baseline index is written
- **THEN** `meta.json` SHALL record the effective RaBitQ-related build parameters
- **AND** those parameters SHALL be sufficient to identify how the codes were produced

### Requirement: The build output SHALL be reproducible and relocatable
The baseline build path SHALL emit a self-describing output directory that can be copied and reopened by the search CLI.

#### Scenario: Output directory contains all required baseline artifacts
- **WHEN** the build command succeeds
- **THEN** the output directory SHALL contain the compressed index plane artifacts and the raw vector plane artifact
- **AND** the output directory SHALL be openable by a later search command without requiring the original input base file

#### Scenario: Build parameters remain visible in metadata
- **WHEN** an experiment script inspects a built output directory
- **THEN** it SHALL be able to recover at least the dataset-independent build controls such as `nlist`, metric, dimensionality, record count, and output version
