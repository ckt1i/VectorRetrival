## ADDED Requirements

### Requirement: ProbeCluster uses FastScan distance for S1 classification
OverlapScheduler::ProbeCluster SHALL use the FastScan batch-32 estimated distances (dists[]) directly for Stage 1 classification and CRC est_heap updates, eliminating the per-vector Popcount loop.

#### Scenario: S1 classification with FastScan distance
- **WHEN** ProbeCluster processes a FastScan block of 32 vectors
- **THEN** it uses dists[] from EstimateDistanceFastScan for ClassifyAdaptive
- **THEN** it does NOT call UnpackSignBitsFromFastScan or PopcountXor

#### Scenario: CRC est_heap uses FastScan distance
- **WHEN** a non-SafeOut vector is classified via FastScan distance
- **THEN** the est_heap is updated with the FastScan distance value (not popcount distance)

#### Scenario: S2 ExRaBitQ unchanged
- **WHEN** a vector is classified as S1 Uncertain
- **THEN** S2 ExRaBitQ processing proceeds identically to current behavior

#### Scenario: Recall preservation
- **WHEN** FastScan classification is used with distance-error-calibrated eps_ip
- **THEN** recall@10 on deep1m is within 0.5% of popcount classification baseline
- **THEN** False SafeOut rate is 0%
