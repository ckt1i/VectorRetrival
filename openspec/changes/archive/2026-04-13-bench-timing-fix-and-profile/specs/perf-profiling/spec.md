## ADDED Requirements

### Requirement: run_perf_profile.sh accepts BENCH_BIN, BENCH_ARGS, OUT_DIR
The script `benchmarks/scripts/run_perf_profile.sh` SHALL accept three positional-or-env parameters:
- `BENCH_BIN`: path to the bench_e2e executable (required)
- `BENCH_ARGS`: arguments to pass to bench_e2e (required, quoted string)
- `OUT_DIR`: output directory (optional, defaults to `/tmp/perf_profile_$(date +%Y%m%dT%H%M%S)`)

#### Scenario: missing BENCH_BIN causes early exit
- **WHEN** `BENCH_BIN` is not provided or the path does not exist
- **THEN** the script SHALL print a usage message and exit with code 1

### Requirement: run_perf_profile.sh runs perf stat with hardware counters
The script SHALL run `perf stat` with counters:
`cycles, instructions, cache-misses, cache-references, branch-misses, LLC-load-misses, LLC-store-misses`
and write the output to `{OUT_DIR}/perf_stat.txt`.

#### Scenario: perf stat succeeds without root
- **WHEN** `perf_event_paranoid` allows user-space perf stat (typically ≤ 2)
- **THEN** the script SHALL capture hardware counters and save to `perf_stat.txt`

#### Scenario: perf stat unavailable degrades gracefully
- **WHEN** `perf` binary is not found in PATH
- **THEN** the script SHALL print a warning ("perf not found, skipping") and continue without stat

### Requirement: run_perf_profile.sh runs perf record for call graph
The script SHALL attempt `perf record -g --call-graph dwarf -o {OUT_DIR}/perf.data` on bench_e2e,
then run `perf report --stdio > {OUT_DIR}/perf_report.txt`.

#### Scenario: perf record requires paranoid <= 1
- **WHEN** `perf_event_paranoid > 1` and no `CAP_PERFMON`
- **THEN** the script SHALL skip perf record, print a note, and exit cleanly

#### Scenario: FlameGraph available generates SVG
- **WHEN** `flamegraph.pl` is found in PATH or in `../FlameGraph/`
- **THEN** the script SHALL run `perf script | stackcollapse-perf.pl | flamegraph.pl > {OUT_DIR}/flamegraph.svg`

### Requirement: run_perf_profile.sh prints analysis hints
After completing all profiling steps, the script SHALL print instructions for interpreting the results:
- which files were generated
- how to read `perf_report.txt` for hotspot functions
- how to open `flamegraph.svg` in a browser

#### Scenario: summary printed after all steps
- **WHEN** all profiling steps complete (with or without perf record)
- **THEN** the script SHALL print a summary block listing output files and next-step instructions
