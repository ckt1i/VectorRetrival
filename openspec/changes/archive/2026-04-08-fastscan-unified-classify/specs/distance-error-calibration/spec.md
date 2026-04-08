## ADDED Requirements

### Requirement: Distance-error-based eps_ip calibration
IvfBuilder SHALL calibrate eps_ip by comparing FastScan estimated distance against true L2 distance, normalized by 2·norm_oc·norm_qc.

#### Scenario: Calibration during index build
- **WHEN** IvfBuilder calibrates eps_ip
- **THEN** it samples (query, target) pairs from each cluster
- **THEN** for each pair it computes: normalized_err = |dist_fastscan - dist_true| / (2·norm_oc·norm_qc)
- **THEN** eps_ip = Pxx(normalized_err) where xx is the configured percentile

#### Scenario: CRC calibration consistency
- **WHEN** CRC CalibrateWithRaBitQ computes per-vector distances
- **THEN** it uses FastScan estimated distances (not popcount distances)
- **THEN** the CRC model is trained on the same distance distribution used during search
