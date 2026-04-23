## ADDED Requirements

### Requirement: The IVF-RaBitQ baseline SHALL expose a parameterized search CLI
The baseline SHALL provide a search interface that can be driven by formal-study style parameter sweeps.

#### Scenario: Search CLI accepts the primary sweep controls
- **WHEN** a user launches the search command
- **THEN** the command SHALL accept at least `--index-dir`, `--queries`, `--raw-vectors`, `--nprobe`, `--candidate-budget`, `--topk`, and `--output`
- **AND** the command SHALL persist those effective controls into its run metadata or output files

### Requirement: Search SHALL execute as official compressed candidate generation followed by exact rerank
The baseline query path SHALL first generate candidates from the official IVF-RaBitQ index and then rerank them using raw vectors from the raw vector plane.

#### Scenario: Official IVF probing precedes exact rerank
- **WHEN** a query is processed
- **THEN** the baseline SHALL load the official IVF-RaBitQ index and execute search with the requested `nprobe`
- **AND** the returned compressed candidates SHALL be treated as the input to exact rerank

#### Scenario: Candidate generation respects the candidate budget
- **WHEN** the compressed search stage has completed
- **THEN** the baseline SHALL expose a global candidate set bounded by `candidate_budget`
- **AND** the final exact rerank stage SHALL operate on that bounded candidate set

#### Scenario: Exact rerank uses raw vectors from the raw vector plane
- **WHEN** the final top-k results are produced
- **THEN** the baseline SHALL load raw vectors for the selected candidate row ids from the raw vector plane
- **AND** it SHALL compute the final ranking using the configured metric rather than the compressed approximation alone

### Requirement: Search outputs SHALL be consumable by the current experiment workflow
The baseline SHALL emit stable outputs that can be wrapped into the current formal-study and warm-serving experiment flows.

#### Scenario: Search output includes ids and timing metrics
- **WHEN** a search batch completes
- **THEN** the baseline SHALL export final top-k ids and timing metrics for the compressed search and rerank phases
- **AND** the output SHALL be sufficient for a wrapper to compute recall and coupled E2E metrics

#### Scenario: Payload fetch can be layered on top without changing search semantics
- **WHEN** the baseline is used in a coupled search-plus-payload evaluation
- **THEN** the payload fetch step SHALL consume the final reranked ids as an external follow-up action
- **AND** the baseline's compressed search and exact rerank semantics SHALL remain unchanged by the choice of payload backend

#### Scenario: `.clu` sidecars are not required as v1 query inputs
- **WHEN** the v1 baseline executes a search run
- **THEN** `cluster_offsets.bin` and `clusters.clu` MAY be validated or audited as sidecar outputs
- **AND** the v1 search path SHALL NOT require direct `.clu` scanning to produce compressed candidates
