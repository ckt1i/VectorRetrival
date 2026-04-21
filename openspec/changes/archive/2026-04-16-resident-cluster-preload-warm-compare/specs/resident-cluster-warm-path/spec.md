## ADDED Requirements

### Requirement: Resident cluster warm path SHALL materialize probe-ready cluster structures before measured queries begin
The system SHALL support a resident warm path that preloads the full `.clu` file and materializes per-cluster probe-ready resident structures before the measured query batch starts.

#### Scenario: Warm-up completes before steady-state query timing starts
- **WHEN** resident cluster warm mode is enabled for a benchmark run
- **THEN** the system SHALL preload the complete `.clu` file before the measured query loop begins
- **AND** it SHALL materialize all probed-cluster quantized-vector views and decoded raw-address mappings during that warm-up phase

### Requirement: Resident warm queries SHALL not perform query-time `.clu` reads or cluster parsing
Once resident warm mode has been prepared, the query hot path SHALL complete compressed-plane search without issuing `.clu` reads or parsing cluster blocks during the measured query loop.

#### Scenario: Resident query path uses only memory-resident cluster state
- **WHEN** a measured query runs after resident warm-up has completed
- **THEN** the system SHALL obtain cluster-side quantized vectors and raw block addresses from resident memory
- **AND** it SHALL NOT issue a fresh `.clu` read
- **AND** it SHALL NOT parse a cluster block during that query

### Requirement: Resident warm path SHALL avoid per-query address-vector copying
The resident warm path SHALL avoid rebuilding query-local cluster objects that copy decoded address vectors from global resident state during measured queries.

#### Scenario: Query uses global resident cluster metadata directly
- **WHEN** a probed cluster is accessed under resident warm mode
- **THEN** the query path SHALL consume globally resident decoded address metadata directly
- **AND** it SHALL NOT create a query-local copy of the same decoded-address vector as part of normal steady-state execution
