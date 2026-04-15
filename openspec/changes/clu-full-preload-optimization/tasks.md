## 1. ClusterStore Full Preload

- [x] 1.1 Audit the current `.clu` loading path in `ClusterStoreReader` and identify the minimum ownership changes needed for a resident full-file preload buffer
- [x] 1.2 Add a full `.clu` preload data structure to `ClusterStoreReader` that can hold the resident file buffer and per-cluster resident views
- [x] 1.3 Implement a preload entry point that reads the complete `.clu` file before query execution and materializes resident cluster views
- [x] 1.4 Expose read-only accessors through `Segment` so the query path can consume resident cluster views without new `.clu` I/O
- [x] 1.5 Add unit or integration coverage that verifies resident cluster views are consistent with the existing parsed-cluster data interpretation

## 2. Query Path Mode Selection

- [x] 2.1 Add an explicit cluster loading mode to the search configuration, with at least `window` and `full_preload`
- [x] 2.2 Keep the current sliding-window `.clu` path intact under `window` mode for rollback and before/after comparison
- [x] 2.3 Update `OverlapScheduler` so `full_preload` mode bypasses `CLUSTER_BLOCK` submissions and consumes resident cluster views directly
- [x] 2.4 Verify that payload and raw-vector body reads still use the existing query-time path under `full_preload`
- [x] 2.5 Add query-path validation that the same search parameters remain runnable under both modes

## 3. Benchmark And Output

- [x] 3.1 Extend `bench_e2e` to accept and report the `.clu` loading mode used for a run
- [x] 3.2 Add benchmark output fields for before/after comparison, including `recall@10`, `e2e_ms`, `p99_ms`, preload time, and preload memory or byte footprint
- [x] 3.3 Run the same warm operating point under `window` and `full_preload` with identical search parameters
- [x] 3.4 Append the comparison results to `baselines/results/e2e_comparison_warm.csv`
- [x] 3.5 Update `baselines/results/analysis.md` with the measured query-speed gain and preload cost

## 4. Comparison And Closeout

- [x] 4.1 Compare the new `full_preload` result against the historical sliding-window result at the same operating point
- [x] 4.2 Decide whether the preload optimization is large enough to justify a refreshed BoundFetch Pareto sweep
- [ ] 4.3 Update the active experiment tracker or follow-up plan with the next-step decision after the before/after benchmark
- [x] 4.4 Document the rollback path and any memory/startup trade-offs discovered during validation
