## ADDED Requirements

### Requirement: SearchStats tracks uring_prep_ms
`SearchStats` SHALL include a `uring_prep_ms` field (double, default 0) that accumulates
the total time spent in `io_uring` SQE preparation calls (`PrepRead()`) inside
`AsyncIOSink::OnCandidate()` during the probe phase.

#### Scenario: uring_prep_ms populated after query
- **WHEN** `OverlapScheduler::Search()` completes a query with at least one cluster probed
- **THEN** `stats().uring_prep_ms` SHALL be > 0 when any SafeIn or Uncertain candidates were submitted

#### Scenario: uring_prep_ms excluded from io_wait
- **WHEN** the query completes in WARM state (all data in page cache)
- **THEN** `stats().io_wait_time_ms` SHALL be < 0.01ms and `stats().uring_prep_ms` SHALL account for SQE prep overhead independently

### Requirement: SearchStats tracks uring_submit_ms
`SearchStats` SHALL include a `uring_submit_ms` field (double, default 0) that accumulates
the time spent in `reader_.Submit()` calls (io_uring ring flush after each cluster probe).

#### Scenario: uring_submit_ms non-zero when IO submitted
- **WHEN** a query submits any io_uring reads during `ProbeAndDrainInterleaved()`
- **THEN** `stats().uring_submit_ms` SHALL be ≥ 0 and SHALL be included in `pipeline_stats` output

### Requirement: SearchStats tracks parse_cluster_ms
`SearchStats` SHALL include a `parse_cluster_ms` field (double, default 0) that accumulates
the time spent in `ParseClusterBlock()` calls inside `DispatchCompletion()`.

#### Scenario: parse_cluster_ms accounts for cluster deserialization
- **WHEN** a cluster block CQE is received and dispatched
- **THEN** `stats().parse_cluster_ms` SHALL increase by the duration of `ParseClusterBlock()`

### Requirement: SearchStats tracks fetch_missing_ms
`SearchStats` SHALL include a `fetch_missing_ms` field (double, default 0) that records
the time spent in `FetchMissingPayloads()` at the end of each query.

#### Scenario: fetch_missing_ms is zero when no missing payloads
- **WHEN** all SafeIn payloads were already fetched during the main pipeline
- **THEN** `stats().fetch_missing_ms` SHALL be 0 or negligible (< 0.001ms)

### Requirement: results.json pipeline_stats includes new timing fields
`bench_e2e` results.json `pipeline_stats` block SHALL include:
`avg_uring_prep_ms`, `avg_uring_submit_ms`, `avg_parse_cluster_ms`, `avg_fetch_missing_ms`
as averages over all measurement queries.

#### Scenario: all timing fields sum to total
- **WHEN** a query completes
- **THEN** `probe_time_ms + io_wait_time_ms + rerank_cpu_ms + parse_cluster_ms + fetch_missing_ms + other` SHALL be ≤ `total_time_ms` (no double counting)
