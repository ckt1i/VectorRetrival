# Experiment Tracker

**Date**: 2026-04-15
**System**: BoundFetch
**Protocol**: Warm steady-state only

## Post-Synchronized Baseline Decision

- Tuned `DiskANN+FlatStor` is now the strongest observed warm-serving frontier on `coco_100k`.
- Current `BoundFetch` still clearly beats the `FAISS-IVFPQ+FlatStor` family, but it is not yet the serving winner against DiskANN.
- The next BoundFetch step is **not** another round of generic submit-path tuning.
- The previous next BoundFetch step **was** warm-resident preload of `.clu` quantized vectors and raw address metadata.
- `DiskANN` stays in the baseline set.
- The future baseline plan is layered; we will not run the full `search x storage` Cartesian matrix by default.

## Post-Preload Direction Decision

- `.clu` `full_preload` is now validated as a real steady-state optimization.
- The remaining gap to `DiskANN+FlatStor` is no longer best explained by cluster-side submit/parse overhead.
- For the `recall@10 >= 0.95` region, the next BoundFetch optimization target is the CPU-side probe / verification path.
- The main paper comparison should now shift toward synchronized IVF-family baselines, while keeping DiskANN as a strong graph upper-bound reference rather than the only primary target.
- Dataset expansion is needed, but only after the COCO 100K comparison story stabilizes.

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
| R042 | M5 | Preload ablation | BoundFetch `preload=off` at current best point | coco_100k | recall@10, e2e_ms, p99_ms, RSS | MUST | DONE | `window` mode at `nlist=2048, nprobe=200, alpha=0.05`: `0.9346 / 2.1224ms / 2.4851ms` |
| R043 | M5 | Preload ablation | BoundFetch `preload_codes` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | MUST | DONE | Current implementation preloads the full `.clu`; measured preload-on point: `0.9346 / 1.6860ms / 2.4900ms`, preload `78.352ms`, resident `.clu` `120.5MB` |
| R044 | M5 | Preload ablation | BoundFetch `preload_codes_and_addr` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | MUST | DONE | Implemented as resident full `.clu` preload, so codes and address metadata are loaded together in this revision |
| R045 | M5 | Breakdown validation | BoundFetch best preload setting | coco_100k | uring_submit_ms, probe_ms, cluster_parse_ms, RSS | MUST | DONE | `uring_submit_ms: 0.8088 -> 0.1290`, `cluster_parse_ms: 0.0616 -> 0.0000`; hot-path gain comes from removing query-time cluster submit/parse overhead |

## M6: Refreshed BoundFetch Pareto

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R046 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9640 / 2.9930ms / 4.4790ms` |
| R047 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9551 / 2.7660ms / 4.2230ms` |
| R048 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Anchor point measured: `0.9346 / 1.6860ms / 2.4900ms` |
| R049 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9163 / 1.8250ms / 3.5830ms` |
| R050 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=200, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9021 / 1.7560ms / 3.5680ms` |
| R051 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9853 / 4.6650ms / 7.0140ms` |
| R052 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9843 / 6.1040ms / 8.7120ms` |
| R053 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9346 / 1.6950ms / 2.5640ms` |
| R054 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9171 / 1.8320ms / 3.7680ms` |
| R055 | M6 | Refreshed frontier | BoundFetch preload-on `nprobe=500, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | `0.9025 / 1.7560ms / 3.8500ms` |
| R056 | M6 | Consolidate refreshed frontier | Merge historical and preload-on BoundFetch fronts | coco_100k | non-dominated points | MUST | DONE | Full preload-on grid completed for `nprobe={200,500}` and `alpha={0.01,0.02,0.05,0.08,0.1}`; refreshed frontier can now be consolidated |

## M6A: Low-Load Preload Retest

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R056A | M6A | Retest anchor point | BoundFetch `window`, `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Low-load retest: `0.9346 / 2.4750ms / 3.2250ms` |
| R056B | M6A | Retest anchor point | BoundFetch `full_preload`, `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms, preload_time, preload_bytes | MUST | DONE | Low-load retest: `0.9346 / 1.6090ms / 1.9460ms`, preload `68.0ms`, resident `.clu` `120.5MB` |
| R056C | M6A | Retest high-recall point | BoundFetch `full_preload`, `nprobe=200, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms, preload_time | MUST | DONE | `0.9551 / 2.3440ms / 2.8340ms` |
| R056D | M6A | Retest high-recall point | BoundFetch `full_preload`, `nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms, preload_time | MUST | DONE | `0.9640 / 2.3660ms / 2.8300ms` |

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

## M9: CPU-Side BoundFetch Optimization

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R067 | M9 | Hot-path profiling refresh | BoundFetch `full_preload`, `alpha=0.02`, `epsilon=0.75`, `bits=4` | coco_100k | probe_ms, rerank_cpu_ms, candidate_count | MUST | DONE | New main anchor: `0.9519 / 1.772ms / 2.078ms`; `perf` shows query-side CPU is dominated by `QuantizeQuery14Bit` and `PrepareQueryRotatedInto`, while whole-benchmark top sample is still GT `L2Sqr` |
| R068 | M9 | CPU optimization pass 1 | BoundFetch + reduced duplicate decode / checks | coco_100k | recall@10, e2e_ms, p99_ms, probe_ms | MUST | TODO | Keep search semantics fixed |
| R069 | M9 | CPU optimization pass 2 | BoundFetch + data-layout / SIMD-friendly probe improvements | coco_100k | recall@10, e2e_ms, p99_ms, probe_ms | MUST | TODO | Only proceed if R068 moves the profile in the right direction |
| R070 | M9 | CPU optimization pass 3 | BoundFetch + candidate materialization / rerank reduction | coco_100k | recall@10, e2e_ms, p99_ms, rerank_cpu_ms | MUST | TODO | Stop if gains are negligible |

## M10: Synchronized IVF-Family Pareto

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R071 | M10 | Baseline curve | Re-tuned FAISS-IVFPQ under final warm protocol | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Refresh curve with the same reporting pipeline as BoundFetch |
| R072 | M10 | Baseline curve | IVF+RQ or ConANN synchronized sweep | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Choose the cheapest credible second IVF-family baseline |
| R073 | M10 | Consolidated IVF-family figure | BoundFetch + FAISS + IVF+RQ/ConANN | coco_100k | non-dominated points | MUST | TODO | This becomes the main family-comparison Pareto figure |

## M11: Dataset Expansion

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R074 | M11 | Scale-up check | BoundFetch best point on `deep1m` or `deep8m` | deep* | recall@10, e2e_ms, build_time | NICE | TODO | Run only after M9-M10 stabilize |
| R075 | M11 | Real-data check | BoundFetch + strongest IVF-family baseline on `MS MARCO Passage` or `Amazon Products` | real-data | recall@10, e2e_ms, build_time | NICE | TODO | Payload heterogeneity and realistic schema |

## M12: FastScan Epsilon Rebuild Validation

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R076 | M12 | Control check | Reuse `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048` with CLI `epsilon=0.90` | coco_100k | loaded_eps_ip, resolved_index_dir | MUST | DONE | `loaded_eps_ip` stays at `0.0995`; confirms CLI epsilon does not override reused index metadata |
| R077 | M12 | Clustering export | Export reusable `fkmeans` clustering artifacts from historical index | coco_100k | centroids.fvecs, assignments.ivecs | MUST | DONE | Exported to `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048/{centroids.fvecs,assignments.ivecs}` |
| R078 | M12 | Rebuild validation | BoundFetch rebuilt `epsilon=0.90` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, uncertain | MUST | DONE | Superseded debugging run: auto-rebuilt as `bits=1`, so Stage 2 was inactive |
| R079 | M12 | Rebuild validation | BoundFetch rebuilt `epsilon=0.95` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, uncertain | MUST | DONE | Superseded debugging run: auto-rebuilt as `bits=1`, so Stage 2 was inactive |
| R080 | M12 | Rebuild validation | BoundFetch rebuilt `epsilon=0.99` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, uncertain | MUST | DONE | Superseded debugging run: auto-rebuilt as `bits=1`, so Stage 2 was inactive |
| R081 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.99` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.99`; `0.0994 / 0.9563 / 2.364ms / 2.865ms`; `safe_out=3994.6`, `s2_safe_out=4375.5` |
| R082 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.95` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.95`; `0.0753 / 0.9561 / 2.050ms / 2.580ms`; `safe_out=6134.9`, `s2_safe_out=2251.8` |
| R083 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.90` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`; `0.0631 / 0.9561 / 1.925ms / 2.411ms`; `safe_out=6951.7`, `s2_safe_out=1449.8` |
| R084 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.85` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.85`; `0.0553 / 0.9556 / 1.857ms / 2.259ms`; `safe_out=7369.1`, `s2_safe_out=1044.7` |
| R085 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.80` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.80`; `0.0492 / 0.9541 / 1.814ms / 2.269ms`; `safe_out=7626.3`, `s2_safe_out=790.2` |
| R086 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.75` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.75`; `0.0442 / 0.9519 / 1.772ms / 2.078ms`; `safe_out=7803.1`, `s2_safe_out=615.7` |
| R087 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.70` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.70`; `0.0398 / 0.9500 / 1.743ms / 2.018ms`; `safe_out=7940.9`, `s2_safe_out=487.5` |
| R088 | M12B | Corrected rebuild | BoundFetch rebuilt `bits=4`, `epsilon=0.60` on exported `fkmeans` clustering | coco_100k | loaded_eps_ip, recall@10, e2e_ms, p99_ms, safe_out, s2_safe_out | MUST | DONE | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.60`; `0.0324 / 0.9409 / 1.706ms / 1.911ms`; `safe_out=8116.8`, `s2_safe_out=315.0` |

## Current Interpretation

- Current best practical BoundFetch preload-on tradeoff: `nprobe=200, alpha=0.05` at `0.9346 / 1.609ms / 1.946ms`
- Current main-table / Pareto anchor: corrected `bits=4`, `nprobe=200`, `alpha=0.02`, `epsilon=0.75` at `0.9519 / 1.772ms / 2.078ms`
- Current best practical BoundFetch preload-on high-recall point: corrected `bits=4`, `nprobe=200`, `alpha=0.02`, `epsilon=0.85` at `0.9556 / 1.857ms / 2.259ms`
- Current stronger preload-on high-recall point: `nprobe=200, alpha=0.01` at `0.9640 / 2.366ms / 2.830ms`
- Current strongest DiskANN point in the low-latency region: `L_search=5` at `0.993 / 1.159ms / 1.901ms`
- Current practical conclusion:
  - BoundFetch beats the observed IVF-family baselines
  - `full_preload` is worth keeping, but it does not remove the high-recall gap to DiskANN
  - the next low-level lever is CPU-side search cost, not more cluster-side I/O-path work
  - synchronized IVF-family tuning is now higher priority than broader dataset expansion
  - runtime FastScan epsilon is confirmed to be a build-time property of the index, not a query-time override knob
  - the earlier `index_fkmeans_2048_eps*` sweep was accidentally rebuilt as `bits=1`, so it is only a debugging record and not the final serving conclusion
  - the corrected `bits=4` sweep shows epsilon changes both Stage 1 `safe_out` volume and real Stage 2 load
  - reusable `fkmeans` clustering artifacts are now exported, so the next epsilon Pareto refresh can stay on the historical clustering and the intended `bits=4` path
  - on the new `epsilon=0.75` anchor, measured query CPU is still the bottleneck: `probe_ms=1.500`, `uring_submit_ms=0.089`, `rerank_cpu_ms=0.012`, and the main query-side hotspots are per-query preparation (`QuantizeQuery14Bit` + `PrepareQueryRotatedInto`) rather than `.clu` I/O

## Decision Constraints

- Do not remove DiskANN from the paper unless a reproducible integration is impossible and that limitation is clearly documented.
- Do not run a full `search x storage` matrix until the layered plan is complete.
- Do not start another round of submit-path micro-optimization unless new profiling evidence contradicts the preload conclusion.
- If `R067-R070` fail to create a useful new high-recall BoundFetch region, freeze low-level optimization and shift effort to the layered tradeoff story.
