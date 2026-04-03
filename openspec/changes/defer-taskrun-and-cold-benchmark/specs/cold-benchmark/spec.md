## ADDED Requirements

### Requirement: bench_e2e cold-read mode
bench_e2e SHALL support a `--cold` flag that performs hot-read vs cold-read comparison within a single run, using posix_fadvise to evict page cache without requiring sudo.

#### Scenario: Cold-read benchmark with --cold flag
- **WHEN** bench_e2e is executed with `--cold` flag and a valid dataset
- **THEN** it runs a full round of queries as hot-read baseline
- **THEN** it calls `posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED)` on both .clu and .dat file descriptors to evict page cache
- **THEN** it immediately runs a second round of queries as cold-read
- **THEN** it prints a comparison of hot vs cold metrics: avg_query_ms, avg_io_wait_ms, avg_probe_ms, overlap_ratio

#### Scenario: Normal benchmark without --cold flag
- **WHEN** bench_e2e is executed without `--cold` flag
- **THEN** behavior is unchanged from current implementation (single round of queries)
