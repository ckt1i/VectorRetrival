## ADDED Requirements

### Requirement: bench_e2e supports --nprobe-sweep parameter
`bench_e2e` SHALL accept `--nprobe-sweep <comma-separated-list>` (e.g. `--nprobe-sweep 50,100,150,200`)
as an alternative to `--nprobe`. When specified, it SHALL run a complete warmup + measurement cycle
for each nprobe value in order and output per-nprobe results.

#### Scenario: nprobe-sweep mutual exclusion with nprobe
- **WHEN** both `--nprobe` and `--nprobe-sweep` are provided
- **THEN** bench_e2e SHALL print an error and exit with code 1

#### Scenario: nprobe-sweep runs each value independently
- **WHEN** `--nprobe-sweep 50,100,200` is specified
- **THEN** bench_e2e SHALL run 3 separate rounds (each with warmup + measurement) using the same pre-built index
- **THEN** each round SHALL NOT share state with adjacent rounds (index reloaded or nprobe-only config change)

### Requirement: nprobe-sweep outputs recall-latency CSV
Each nprobe sweep run SHALL append one row to `{output_dir}/nprobe_sweep.csv` with columns:
`nprobe, recall@1, recall@10, avg_ms, p50_ms, p99_ms, avg_probe_ms, avg_io_wait_ms, avg_safe_out_rate`.

#### Scenario: CSV created on first run
- **WHEN** `nprobe_sweep.csv` does not exist in output_dir
- **THEN** bench_e2e SHALL create the file with header row before writing data

#### Scenario: CSV appended on subsequent runs
- **WHEN** `nprobe_sweep.csv` already exists (from a prior run)
- **THEN** bench_e2e SHALL append new rows without overwriting the header

### Requirement: nprobe-sweep prints per-nprobe summary to stdout
For each nprobe value, bench_e2e SHALL print a one-line summary:
`[sweep] nprobe=N  recall@10=X.XXXX  avg=X.XXXms  probe=X.XXXms  safe_out_rate=XX.X%`

#### Scenario: stdout summary visible during long sweep
- **WHEN** a sweep nprobe value finishes its measurement round
- **THEN** the summary line SHALL be printed before the next nprobe starts
