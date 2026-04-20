# Spec: RAIR Secondary Assignment

## Overview

RAIR (Residual-Aware Improved Assignment) is a secondary cluster assignment mode for redundant IVF that selects the secondary cluster by minimizing an AIR-style residual loss rather than simply choosing the second-nearest centroid.

## Requirements

### Requirement: IVF builder SHALL support RAIR-based secondary assignment
The IVF builder SHALL support an assignment mode in which the primary cluster remains the nearest centroid and the secondary cluster for `top-2` redundant assignment is selected by minimizing an AIR-style residual loss rather than by choosing the second-nearest centroid.

The secondary-assignment rule SHALL be configurable independently from whether redundant assignment is enabled, so that `single`, `redundant_top2_naive`, and `redundant_top2_rair` remain distinguishable build modes.

#### Scenario: Primary cluster remains nearest-centroid assignment
- **WHEN** the builder runs in `redundant_top2_rair` mode
- **THEN** the primary cluster for each vector SHALL remain the nearest centroid under the configured distance metric
- **AND** only the secondary cluster selection rule SHALL change relative to the naive redundant mode

#### Scenario: Secondary cluster is selected by AIR loss
- **WHEN** the builder runs in `redundant_top2_rair` mode
- **THEN** it SHALL evaluate secondary-cluster candidates using a residual-aware AIR loss that depends on both residual magnitude and residual direction
- **AND** the chosen secondary cluster SHALL be the candidate with the minimum configured AIR loss

### Requirement: RAIR configuration SHALL be explicit and observable
The system SHALL expose the RAIR-specific configuration needed to reproduce a build and to distinguish it from naive redundant assignment.

#### Scenario: Builder configuration exposes RAIR parameters
- **WHEN** a caller requests `redundant_top2_rair`
- **THEN** the builder configuration SHALL accept the RAIR mode and its tuning parameters, including at least `lambda`
- **AND** the configuration SHALL preserve a rollback path to `single` and `redundant_top2_naive`

#### Scenario: Built index records RAIR assignment mode
- **WHEN** an index is built in `redundant_top2_rair` mode
- **THEN** the persisted index metadata SHALL record the assignment mode
- **AND** the persisted metadata SHALL make the RAIR parameters recoverable for benchmark and diagnostic tooling

### Requirement: RAIR mode SHALL remain compatible with existing redundant serving semantics
Introducing RAIR SHALL not change the query-visible semantics of duplicated postings. A RAIR-built index SHALL continue to use the existing redundant serving path, including unique final-vector semantics and duplicate-candidate collapse before downstream fetch.

#### Scenario: Query-visible results remain unique by original vector identity
- **WHEN** a query runs against an index built with `redundant_top2_rair`
- **THEN** the final top-k results SHALL remain unique by original vector identity
- **AND** duplicated postings SHALL not create duplicate user-visible records

#### Scenario: RAIR uses existing duplicated-posting storage path
- **WHEN** the builder emits a `redundant_top2_rair` index
- **THEN** duplicated postings SHALL be materialized through the existing redundant-posting storage path
- **AND** the build SHALL remain compatible with the current `.clu V9` raw-address-table format

## Assignment Modes

| Mode | Primary | Secondary |
|------|---------|-----------|
| `single` | nearest centroid | none |
| `redundant_top2_naive` | nearest centroid | second-nearest centroid |
| `redundant_top2_rair` | nearest centroid | minimum AIR loss candidate |
