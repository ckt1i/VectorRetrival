# Experiment Tracker

**Date**: 2026-04-14
**System**: BoundFetch
**Protocol**: Warm steady-state only

## Post-Synchronized Baseline Decision

- Tuned `DiskANN+FlatStor` is now the strongest observed warm-serving frontier on `coco_100k`.
- Current `BoundFetch` still clearly beats the `FAISS-IVFPQ+FlatStor` family, but it is not yet the serving winner against DiskANN.
- The next BoundFetch step is **not** another round of generic submit-path tuning.
- The next BoundFetch step **is** warm-resident preload of `.clu` quantized vectors and raw address metadata.
- `DiskANN` stays in the baseline set.
- The future baseline plan is layered; we will not run the full `search x storage` Cartesian matrix by default.

## Synchronization Rule

- All Pareto runs must use the same warm-only protocol on `coco_100k`.
- All systems must use `queries=1000` and `topk=10`.
- DiskANN remains a required strong reference even when it currently dominates the frontier.
- Baseline expansion must follow the layered policy:
  - main search-core comparison
  - storage-backend ablation
  - build/preload cost table

## Historical Main Warm Pareto Results

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R002 | M1 | Main Pareto anchor | BoundFetch `nlist=2048, nprobe=50, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.8068 / 0.743ms / 1.001ms |
| R003 | M1 | Main Pareto anchor | BoundFetch `nlist=2048, nprobe=100, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.8781 / 1.117ms / 1.513ms |
| R004 | M1 | Main Pareto anchor | BoundFetch `nlist=2048, nprobe=200, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.9021 / 1.409ms / 2.493ms |
| R005 | M1 | Main Pareto anchor | BoundFetch `nlist=2048, nprobe=300, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.9025 / 1.393ms / 2.824ms |
| R006 | M1 | Main Pareto anchor | BoundFetch `nlist=2048, nprobe=500, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.9025 / 1.403ms / 2.773ms |
| R012 | M3 | Recall-improvement best point | BoundFetch `nlist=2048, nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.9346 / 1.511ms / 1.855ms |
| R021 | M1A | High-recall point | BoundFetch `nlist=2048, nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.9640 / 2.251ms / 2.648ms |
| R022 | M1A | High-recall point | BoundFetch `nlist=2048, nprobe=500, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.9853 / 3.788ms / 4.313ms |

## Historical Baseline Pareto Results

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R025 | M1B | Baseline curve | DiskANN+FlatStor `L_search=5` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.993 / 1.159ms / 1.901ms |
| R026 | M1B | Baseline curve | DiskANN+FlatStor `L_search=10` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.993 / 1.374ms / 2.187ms |
| R027 | M1B | Baseline curve | DiskANN+FlatStor `L_search=15` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.995 / 1.395ms / 2.223ms |
| R028 | M1B | Baseline curve | DiskANN+FlatStor `L_search=20` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.997 / 1.925ms / 2.975ms |
| R033 | M1B | Baseline curve | FAISS-IVFPQ+FlatStor `nprobe=8` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.715 / 0.333ms / 0.491ms |
| R034 | M1B | Baseline curve | FAISS-IVFPQ+FlatStor `nprobe=16` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.745 / 0.657ms / 0.935ms |
| R035 | M1B | Baseline curve | FAISS-IVFPQ+FlatStor `nprobe=32` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | 0.747 / 1.317ms / 1.658ms |
| R041 | M1B | Consolidated Pareto table | BoundFetch + DiskANN + FAISS | coco_100k | non-dominated points | MUST | DONE | Consolidated CSV at `baselines/results/` |

## Historical Mechanism Attribution

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R010 | M2 | Mechanism attribution | BoundFetch `nprobe=200, alpha=0.1` | coco_100k | uring_submit_ms, probe_ms, io_wait_ms, submit_calls | MUST | DONE | `uring_submit_ms` dominates; `io_wait_ms` negligible |
| R011 | M2 | Stability check | BoundFetch `nprobe=300, alpha=0.1` | coco_100k | same as R010 | MUST | DONE | same conclusion |

## M5: Warm-Resident Preload Implementation

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R042 | M5 | Preload ablation | BoundFetch `preload=off` at current best point | coco_100k | recall@10, e2e_ms, p99_ms, RSS | MUST | TODO | Baseline for preload comparison |
| R043 | M5 | Preload ablation | BoundFetch `preload_codes` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | MUST | TODO | Cache quantized vectors only |
| R044 | M5 | Preload ablation | BoundFetch `preload_codes_and_addr` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | MUST | TODO | Cache quantized vectors and raw address metadata |
| R045 | M5 | Breakdown validation | BoundFetch best preload setting | coco_100k | uring_submit_ms, probe_ms, cluster_parse_ms, RSS | MUST | TODO | Verify what moved on the hot path |

## M6: Refreshed BoundFetch Pareto

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R046 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | High-recall anchor with preload |
| R047 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Near-high-recall point |
| R048 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Current best tradeoff with preload |
| R049 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Frontier density |
| R050 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Compare against historical default |
| R051 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Highest-recall anchor with preload |
| R052 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | High-recall point |
| R053 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Tradeoff point |
| R054 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Frontier density |
| R055 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Compare against historical default |
| R056 | M6 | Consolidate refreshed frontier | Merge historical and preload-on BoundFetch fronts | coco_100k | non-dominated points | MUST | TODO | Decision gate for further optimization |

## M7: Layered Baseline Comparison

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R057 | M7 | Main search-core table | Choose main backend policy | coco_100k | comparability note | MUST | TODO | Prefer shared `Lance` backend if feasible |
| R058 | M7 | Main search-core table | BoundFetch best preload point on chosen policy | coco_100k | recall@10, e2e_ms, p99_ms, build_time | MUST | TODO | Main table point |
| R059 | M7 | Main search-core table | IVF+PQ or IVF+RQ on chosen policy | coco_100k | recall@10, e2e_ms, p99_ms, build_time | MUST | TODO | Lower classical baseline |
| R060 | M7 | Main search-core table | DiskANN on chosen policy or as `DiskANN+FlatStor` upper reference | coco_100k | recall@10, e2e_ms, p99_ms, build_time | MUST | TODO | Must remain in scope |
| R061 | M7 | Storage ablation | BoundFetch across `FlatStor` and `Lance` | coco_100k | recall@10, e2e_ms, startup_time, file_size | MUST | TODO | Start with 2 backends only |
| R062 | M7 | Storage ablation | IVF baseline across `FlatStor` and `Lance` | coco_100k | recall@10, e2e_ms, startup_time, file_size | MUST | TODO | Separate backend effect from search-core effect |
| R063 | M7 | Optional storage ablation | Add `Parquet` if integration is cheap | coco_100k | recall@10, e2e_ms, startup_time, file_size | NICE | TODO | Do not block the main story |

## M8: Build and Startup Cost Table

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R064 | M8 | Build-cost table | BoundFetch build and preload cost | coco_100k | build_time, peak_RSS, index_bytes, preload_bytes, preload_time | MUST | TODO | Needed even if DiskANN still wins serving |
| R065 | M8 | Build-cost table | DiskANN build and startup cost | coco_100k | build_time, peak_RSS, index_bytes, preload_bytes, preload_time | MUST | TODO | Strong comparison axis |
| R066 | M8 | Build-cost table | IVF-family baseline build and startup cost | coco_100k | build_time, peak_RSS, index_bytes, preload_bytes, preload_time | MUST | TODO | Completes tradeoff table |

## Current Interpretation

- Current best BoundFetch tradeoff: `nprobe=200, alpha=0.05` at `0.9346 / 1.511ms / 1.855ms`
- Current best BoundFetch high-recall point: `nprobe=200, alpha=0.01` at `0.9640 / 2.251ms / 2.648ms`
- Current strongest DiskANN point in the low-latency region: `L_search=5` at `0.993 / 1.159ms / 1.901ms`
- Current practical conclusion:
  - BoundFetch beats IVF-family baselines
  - DiskANN remains the warm-serving upper baseline
  - preload is the highest-value next test

## Decision Constraints

- Do not remove DiskANN from the paper unless a reproducible integration is impossible and that limitation is clearly documented.
- Do not run a full `search x storage` matrix until the layered plan is complete.
- Do not start another round of submit-path micro-optimization before `R042-R056` finish.
- If `R042-R056` fail to create a useful new BoundFetch frontier region, freeze low-level optimization and shift effort to the layered tradeoff story.
