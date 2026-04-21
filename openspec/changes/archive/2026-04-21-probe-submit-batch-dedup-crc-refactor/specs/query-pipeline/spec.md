## MODIFIED Requirements

### Requirement: Query pipeline optimization work SHALL preserve the existing phase-based attribution boundary
The query pipeline SHALL preserve the existing phase-based attribution boundary when follow-up optimizations are analyzed or implemented, so that later changes remain comparable with the current breakdown.

#### Scenario: Follow-up optimization remains attributable
- **WHEN** a later optimization changes the query path
- **THEN** the analysis and validation SHALL remain attributable against the existing phase breakdown
- **AND** the optimization SHALL identify which phase or sub-phase it is expected to reduce

#### Scenario: `probe_submit` refactor remains inside submit-path attribution
- **WHEN** the `probe_submit` path is refactored into batched submit, batch-aware dedup, or cluster-end CRC merge
- **THEN** the resulting work SHALL remain attributable to the submit-path portion of the query breakdown
- **AND** it SHALL NOT be reported as `probe_prepare_ms`, `probe_stage1_ms`, or `probe_stage2_ms`
