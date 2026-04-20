## ADDED Requirements

### Requirement: Faiss coarse builder SHALL reproduce historical Python IVF coarse-training semantics
When `coarse_builder=faiss_kmeans` is used without precomputed clustering artifacts, the system SHALL build coarse centroids and primary assignments using the same Faiss IVF coarse-training semantics as the historical Python exporter path rather than a divergent approximate reimplementation.

#### Scenario: In-process Faiss build follows IVF coarse-training semantics
- **WHEN** an index build requests `coarse_builder=faiss_kmeans` and does not provide `centroids_path` plus `assignments_path`
- **THEN** the builder SHALL train coarse centroids through Faiss IVF coarse-training logic
- **AND** it SHALL export centroids from the trained quantizer
- **AND** it SHALL derive primary assignments from the trained quantizer's nearest-centroid search

#### Scenario: IP-effective coarse training uses spherical Faiss clustering
- **WHEN** the requested coarse metric resolves to effective inner-product semantics
- **THEN** the Faiss coarse-training configuration SHALL use the corresponding Faiss IVF inner-product clustering behavior
- **AND** the resulting in-process path SHALL not silently fall back to non-spherical L2-style centroid updates

### Requirement: Faiss coarse builder SHALL preserve explicit effective-metric provenance
The system SHALL persist enough build provenance to reconstruct the exact coarse-training semantics used by the in-process Faiss builder.

#### Scenario: Build metadata records Faiss parity settings
- **WHEN** an index is built with `coarse_builder=faiss_kmeans`
- **THEN** the emitted build metadata SHALL include the requested metric
- **AND** it SHALL include the effective coarse metric
- **AND** it SHALL include Faiss training size, iteration count, restart count, and backend

### Requirement: Query-time centroid probing SHALL follow the stored effective coarse metric
Indexes built with Faiss coarse partitions SHALL rank coarse centroids at query time using the same effective metric family used to train those centroids.

#### Scenario: IP-effective indexes probe centroids with IP-consistent ranking
- **WHEN** an opened index records effective coarse metric `ip`
- **THEN** `FindNearestClusters()` SHALL rank centroids with inner-product-consistent ordering
- **AND** it SHALL apply the same query normalization rule used by the builder when the requested metric is cosine

#### Scenario: L2-effective indexes retain L2 centroid probing
- **WHEN** an opened index records effective coarse metric `l2`
- **THEN** `FindNearestClusters()` SHALL continue to rank centroids by L2 distance

### Requirement: Faiss parity acceptance SHALL compare against historical Python-generated artifacts
The in-process Faiss builder SHALL not be accepted as parity-preserving unless it is evaluated against the historical Python-precomputed Faiss artifacts under the same benchmark family.

#### Scenario: Regression check compares in-process and precomputed Faiss paths
- **WHEN** the Faiss parity regression is run on `coco_100k / nlist=2048 / topk=10 / single`
- **THEN** it SHALL compare the in-process C++ Faiss builder path against the historical Python-precomputed Faiss artifact path
- **AND** it SHALL report whether the two paths remain in the same operating-point tier for `recall@10`

#### Scenario: Material parity failure is treated as a blocker
- **WHEN** the in-process Faiss path requires a meaningfully larger `nprobe` than the historical Python-precomputed path to reach the target operating point
- **THEN** the regression outcome SHALL be treated as a parity failure rather than accepted drift
