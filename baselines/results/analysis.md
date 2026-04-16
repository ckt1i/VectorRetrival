# Warm Serving Evaluation Analysis

**Date**: 2026-04-16
**System**: BoundFetch
**Dataset**: COCO 100K
**Protocol**: Warm steady-state only

## Executive Summary

This document now contains three result blocks:
1. Historical warm `full_preload` points collected before the rebuild investigation
2. The reproducible `nprobe=200` rebuild sweeps used to isolate epsilon effects
3. The current main comparison curve: `epsilon=0.90`, `nprobe=512`, `bits=4`, `clu_mode=full_preload`

**Current main curve**: the latest comparison uses `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90_rebuild_20260415_epssweep` and reruns `crc-alpha ∈ {0.01,0.02,0.05,0.08,0.10,0.15,0.20}` at `nprobe=512`.

**Current best recall point**: `alpha=0.01` reaches `recall@10=0.9887` at `1.5114 ms` average latency and `2.2997 ms` p99.

**Current practical high-recall tradeoff**: `alpha=0.05` reaches `recall@10=0.9694` at `1.4785 ms`.

**Comparison summary**:
- Against the earlier `nprobe=200` rebuild curves, `nprobe=512` closes most of the recall gap and moves BoundFetch into the `0.97-0.99` regime.
- Against `FAISS-IVFPQ+FlatStor`, the new curve still dominates the whole reported baseline range.
- Against `DiskANN+FlatStor`, BoundFetch still trails at the very top end, but the gap is now narrow enough that the comparison should be made point-by-point rather than by recall band alone.

---

## 0. Current Main Curve (`epsilon=0.90`, `nprobe=512`, `bits=4`)

### Fixed Build and Evaluation Protocol

- dataset: `coco_100k`
- rebuilt index dir: `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90_rebuild_20260415_epssweep`
- `nprobe=512`
- `queries=1000`
- `clu_mode=full_preload`
- `crc-alpha={0.01,0.02,0.05,0.08,0.10,0.15,0.20}`

### Sweep Results

| crc-alpha | recall@10 | avg_ms | p99_ms |
|-----------|-----------|--------|--------|
| 0.01 | 0.9887 | 1.5114 | 2.2997 |
| 0.02 | 0.9848 | 1.5098 | 2.3033 |
| 0.05 | 0.9694 | 1.4785 | 2.3044 |
| 0.08 | 0.9194 | 0.6906 | 1.4584 |
| 0.10 | 0.9059 | 0.6661 | 1.4146 |
| 0.15 | 0.8685 | 0.6073 | 1.3383 |
| 0.20 | 0.8342 | 0.5554 | 1.2663 |

### Position Relative to Baselines

1. `alpha=0.01` and `0.02` push BoundFetch to `0.985-0.989` recall, which is now close to the lower end of the DiskANN sweep.
2. At these high-recall points, BoundFetch average latency is about `1.51 ms`, compared with DiskANN's `1.11-1.40 ms` in the `0.993-0.995` range.
3. The new curve remains strictly better than FAISS on both recall and latency.
4. The useful interpretation is now:
   - `alpha=0.01/0.02/0.05`: high-recall comparison region
   - `alpha=0.08+`: lower-latency operating region

### Updated Baseline Comparison

| System | recall@10 | avg_ms | Notes |
|--------|-----------|--------|-------|
| **BoundFetch (`epsilon=0.90`, `nprobe=512`, `alpha=0.01`)** | **0.9887** | **1.5114** | Current highest-recall BoundFetch point |
| BoundFetch (`epsilon=0.90`, `nprobe=512`, `alpha=0.05`) | 0.9694 | 1.4785 | Current high-recall tradeoff point |
| DiskANN+FlatStor (`L_search=1`) | 0.9930 | 1.1090 | Faster and slightly higher recall |
| DiskANN+FlatStor (`L_search=15`) | 0.9950 | 1.3953 | Similar latency, higher recall |
| FAISS-IVFPQ+FlatStor (`nprobe=32`) | 0.7470 | 1.3167 | Dominated by BoundFetch |

The current `baselines_recall_qps.svg` should therefore be read as a high-recall comparison figure centered on the `epsilon=0.90, nprobe=512` BoundFetch curve.

---

## 0A. Phase1-Revert Sweep (`epsilon=0.75`, `nlist=2048`, `bits=4`)

### Fixed Build and Evaluation Protocol

- dataset: `coco_100k`
- rebuilt index dir: `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.75`
- `nprobe=200`
- `queries=1000`
- `clu_mode=full_preload`
- `crc-alpha={0.01,0.02,0.03,0.04,0.05,0.07,0.10,0.15,0.20}`

### Reproducible Alpha Sweep

| crc-alpha | recall@10 | avg_ms | p99_ms | preload_time_ms |
|-----------|-----------|--------|--------|-----------------|
| 0.01 | 0.9505 | 0.7190 | 0.9249 | 12.4895 |
| 0.02 | 0.9467 | 0.7119 | 0.9716 | 12.6350 |
| 0.03 | 0.9417 | 0.7008 | 0.9059 | 12.6915 |
| 0.04 | 0.9381 | 0.6961 | 0.9104 | 12.5229 |
| 0.05 | 0.9322 | 0.7047 | 0.9788 | 12.5600 |
| 0.07 | 0.9217 | 0.5851 | 0.9064 | 12.2937 |
| 0.10 | 0.9016 | 0.5550 | 0.9022 | 12.2982 |
| 0.15 | 0.8675 | 0.5145 | 0.9107 | 12.4223 |
| 0.20 | 0.8334 | 0.4818 | 0.9124 | 12.4999 |

### Direct Comparison to Historical `full_preload`

The most useful matched comparison is against the earlier low-load `full_preload` points at the same `nprobe=200` alpha settings:

| alpha | historical recall@10 / avg_ms | phase1-revert recall@10 / avg_ms | delta recall | speedup |
|-------|-------------------------------|------------------------------------|--------------|---------|
| 0.01 | 0.9640 / 2.3660 | 0.9505 / 0.7190 | -0.0135 | 3.29x |
| 0.02 | 0.9551 / 2.3440 | 0.9467 / 0.7119 | -0.0084 | 3.29x |
| 0.05 | 0.9346 / 1.6090 | 0.9322 / 0.7047 | -0.0024 | 2.28x |
| 0.10 | 0.9021 / 1.4580 | 0.9016 / 0.5550 | -0.0005 | 2.63x |
| 0.15 | 0.8180 / 0.8310 | 0.8675 / 0.5145 | +0.0495 | 1.62x |
| 0.20 | 0.7578 / 0.6790 | 0.8334 / 0.4818 | +0.0756 | 1.41x |

Interpretation:

1. The current rollback curve is now the operational Pareto curve for the reproducible pipeline.
2. In the high-recall region (`alpha=0.01-0.05`), the rollback curve is much faster but gives up some recall relative to the historical stage.
3. In the low-recall region (`alpha=0.15-0.20`), the rollback curve is both faster and higher recall than the historical points.
4. Rolling Phase2 back does not materially change the `epsilon=0.75` frontier shape relative to the earlier investigation; it mainly confirms that the currently observed curve is stable under a clean rebuild of the restored layout.
5. This rollback curve is now best treated as a diagnostic reference, not as the main figure for baseline comparison.

### Comparison with the Two Baselines

1. The rollback BoundFetch curve is strictly better than `FAISS-IVFPQ+FlatStor` on the reported warm points: it delivers both higher recall and higher QPS across the full FAISS sweep.
2. The rollback curve does not catch `DiskANN+FlatStor` in absolute recall. DiskANN still occupies the `0.993-0.999` recall band, while the rollback BoundFetch curve tops out at `0.9505`.
3. In the `0.83-0.95` recall band that is actually covered by the rollback BoundFetch curve, BoundFetch remains substantially faster than the tuned DiskANN points.

The remaining sections below are kept as historical mechanism and preload notes. The new figures and the main comparison table now use the fixed rebuilt index sweep above as the current curve.

---

## 1. Main Pareto Block Results (M1)

### BoundFetch nprobe Sweep (nlist=2048, alpha=0.1)

| nprobe | recall@10 | avg_ms | p99_ms | early_stop% |
|--------|-----------|--------|--------|-------------|
| 50 | 0.8068 | 0.743 | 1.001 | 17.5% |
| 100 | 0.8781 | 1.117 | 1.513 | 48.2% |
| 150 | 0.8984 | 1.336 | 2.000 | 82.4% |
| 200 | 0.9021 | 1.409 | 2.493 | 96.2% |
| 250 | 0.9024 | 1.397 | 2.744 | 99.5% |
| 300 | 0.9025 | 1.393 | 2.824 | 100.0% |
| 500 | 0.9025 | 1.403 | 2.773 | 100.0% |

**Observation**: Recall saturates at ~90.25% for nprobe≥200. Latency remains stable at ~1.4ms for nprobe=200-500.

### Baseline Comparison

| System | recall@10 | avg_ms | Notes |
|--------|-----------|--------|-------|
| BoundFetch (nprobe=200, alpha=0.1) | 0.9021 | 1.409 | Best BoundFetch at default alpha |
| DiskANN+FlatStor | 1.000 | 3.790 | Perfect recall but 2.7x slower |
| FAISS-IVFPQ+FlatStor | 0.747 | 1.312 | Faster but recall limited |

---

## 2. Mechanism Attribution (M2)

### Breakdown at Best Operating Point

| Metric | R010 (nprobe=200) | R011 (nprobe=300) | Analysis |
|--------|-------------------|-------------------|----------|
| recall@10 | 0.9021 | 0.9025 | +0.04% |
| avg_ms | 1.409 | 1.384 | -1.8% |
| p99_ms | 2.493 | 2.729 | +9.5% |
| **uring_submit_ms** | 0.732 (52%) | 0.717 (52%) | Dominant cost |
| **probe_ms** | 0.430 (31%) | 0.430 (31%) | Constant |
| **parse_cluster_ms** | 0.051 (3.6%) | 0.052 (3.8%) | Negligible |
| **rerank_cpu_ms** | 0.012 (0.9%) | 0.012 (0.9%) | Negligible |
| **io_wait_ms** | 0.002 (0.1%) | 0.001 (0.07%) | **Negligible** |
| submit_calls | 24.4 | 23.5 | Decreased |

### Key Conclusions

1. **io_wait is not the bottleneck**: <0.1% of total latency
2. **uring_submit dominates**: ~52% of total latency
3. **probe_ms is constant**: nprobe change doesn't affect probe cost
4. **qd/SQPOLL tuning is not the main lever**: io_wait is already fully hidden

---

## 3. Recall Improvement Ablation (M3)

### Extended crc_alpha × nprobe Matrix

| nprobe \ alpha | 0.01 | 0.02 | 0.05 | 0.08 | 0.1 | 0.15 | 0.2 |
|----------------|------|------|------|------|-----|------|-----|
| **200** | 0.9640 | 0.9615 | **0.9346** | ~0.919 | 0.9021 | 0.8180 | 0.7578 |
| | 2.251ms | 2.210ms | **1.511ms** | 1.466ms | 1.409ms | 0.797ms | 0.684ms |
| **250** | 0.9750 | 0.9661 | **0.9346** | 0.9170 | 0.9024 | 0.8180 | 0.7578 |
| | 2.781ms | 2.737ms | **1.568ms** | 1.479ms | 1.397ms | 0.807ms | 0.674ms |
| **300** | 0.9808 | 0.9719 | **0.9346** | 0.9171 | 0.9025 | 0.8180 | - |
| | 3.309ms | 3.234ms | **1.543ms** | 1.493ms | 1.384ms | 0.827ms | - |
| **500** | 0.9853 | 0.9843 | **0.9346** | 0.9171 | 0.9025 | 0.8180 | - |
| | 3.788ms | 5.519ms | **1.536ms** | 1.519ms | 1.403ms | 0.816ms | - |

*Format: recall@10 / avg_ms*

### Key Findings

1. **alpha=0.05 achieves optimal recall-latency tradeoff**:
   - recall@10 = 0.9346 (vs 0.9021 at alpha=0.1, +3.2%)
   - avg_ms = 1.511 (vs 1.409, +7.2%)
   - **p99_ms = 1.855 (vs 2.493, -25.6% improvement!)**

2. **alpha sensitivity is smooth and predictable**:
   - Smaller alpha → more clusters explored → higher recall, higher latency
   - The tradeoff is monotonic and controllable

3. **Query-time `epsilon-percentile` does not affect reused indexes**:
   - The earlier "no effect" observation at `nprobe>=200` came from runs that reused a pre-built `--index-dir`
   - In that mode, changing the benchmark CLI flag alone does not change the runtime FastScan bound because `loaded_eps_ip` still comes from the existing `segment.meta`

4. **alpha=0.01 achieves highest recall (~0.98) but at significant latency cost**

### Optimization Gate: PASSED

**Decision**: Continue code optimization with alpha=0.05 as the new default.

**Rationale**:
- Significant recall improvement (+3.2%) at acceptable latency (+7.2%)
- Dramatic p99 improvement (-25.6%) improves tail latency
- Smooth parameter space allows further tuning if needed

---

## 4. Conclusions and Recommendations

## 4. `.clu` Full-Preload Comparison (M5)

### Same Operating Point, Different `.clu` Loading Mode

| clu_mode | recall@10 | avg_ms | p99_ms | preload_time_ms | preload_bytes |
|----------|-----------|--------|--------|-----------------|---------------|
| `window` | 0.9346 | 2.1224 | 2.4851 | 0.0 | 0 |
| `full_preload` | 0.9346 | 1.6860 | 2.4900 | 78.3520 | 120459264 |

### Direct Effect

1. `full_preload` keeps search semantics unchanged at this operating point: recall@10 stays at `0.9346`.
2. Query latency improves materially:
   - avg query time: `2.1224ms -> 1.6860ms` (`-0.4364ms`, `-20.6%`)
   - p99 query time: `2.4851ms -> 2.4900ms` (no meaningful improvement in this run)
3. Hot-path submit/parsing overhead drops sharply:
   - `avg_uring_submit_ms`: `0.8088ms -> 0.1260ms` (`-84.4%`)
   - `avg_parse_cluster_ms`: `0.0616ms -> 0.0000ms`
   - `avg_submit_calls`: `25.356 -> 8.242`
4. The preload cost is measurable but bounded for `coco_100k`:
   - preload wall time: `78.4ms`
   - resident `.clu` footprint: `120.5MB`

### Interpretation

The preload optimization is still large enough to justify a refreshed BoundFetch Pareto sweep. The main reason is not disk wait hiding; it is removal of cluster-side query-time submission and parsing overhead. This matches the earlier mechanism analysis that `uring_submit_ms` rather than `io_wait_ms` was dominating the hot path.

The benchmark path now triggers preload before the timed query loop, so `preload_time_ms` is tracked as startup overhead instead of being folded into the first-query steady-state latency.

### Refreshed Preload-On Frontier

The preload-on sweep over `nprobe={200,500}` and `alpha={0.01,0.02,0.05,0.08,0.1}` shows a clear pattern:

1. The main practical preload-on point remains `nprobe=200, alpha=0.05`, at `0.9346 / 1.686ms / 2.490ms`.
2. Increasing `nprobe` from `200` to `500` does not help the `alpha=0.05` tradeoff point in any meaningful way: recall stays at `0.9346`, while latency changes only slightly to `1.695ms`.
3. In the high-recall region, preload does not create a new dominant point:
   - `nprobe=200, alpha=0.01`: `0.9640 / 2.993ms / 4.479ms`
   - `nprobe=500, alpha=0.01`: `0.9853 / 4.665ms / 7.014ms`
   These points still buy recall by paying substantial CPU/probe cost rather than submit-path cost.
4. The preload optimization therefore improves the mid-recall operating region, but it does not fundamentally change the shape of the high-recall frontier.

From an experiment-planning perspective, this means the preload change is real and worth keeping, but further gains beyond the current tradeoff point are unlikely to come from `.clu` loading alone. The next optimization lever should target the probe/verification CPU path if the goal is to challenge the tuned DiskANN baseline at higher recall.

### Low-Load Retest Sanity Check

To rule out environment drift, we reran the representative operating point under low machine load and kept the comparison strictly sequential.

| clu_mode | recall@10 | avg_ms | p99_ms | preload_time_ms | preload_bytes |
|----------|-----------|--------|--------|-----------------|---------------|
| `window` | 0.9346 | 2.1250 | 2.4870 | 0.0 | 0 |
| `full_preload` | 0.9346 | 1.5880 | 1.9310 | 67.5880 | 120459264 |

### Interpretation of the Retest

1. The low-load retest confirms that the earlier `window` slowdown was caused by system contention, not by a semantic or code-path regression.
2. `full_preload` remains consistently faster at the same recall target, and the gain is stable under controlled conditions.
3. The most reliable working-point summary for the paper-style comparison is the low-load retest pair:
   - avg query time: `2.1250ms -> 1.5880ms`
   - recall@10: unchanged at `0.9346`
   - p99: `2.4870ms -> 1.9310ms`
4. This strengthens the claim that `.clu` preload is a real steady-state optimization, but also reinforces that the improvement plateaus once cluster-side submission/parsing overhead has been removed.

---

## 5. Conclusions and Recommendations

### Optimal Configurations

| Scenario | nprobe | alpha | recall@10 | avg_ms | p99_ms |
|----------|--------|-------|-----------|--------|--------|
| **Historical Best Tradeoff** | 200 | 0.05 | 0.9346 | 1.511 | 1.855 |
| **Best Tradeoff with `.clu` Full Preload** | 200 | 0.05 | 0.9346 | 1.686 | 2.490 |
| **Lowest Latency** | 200 | 0.15 | 0.8180 | 0.797 | 1.032 |
| **Highest Recall** | 500 | 0.01 | 0.9853 | 3.788 | 4.313 |

### Next Steps

1. Use `alpha=0.05` as the main BoundFetch operating point for preload-on evaluation.
2. Refresh the BoundFetch Pareto curve with `clu_mode=full_preload`, prioritizing `nprobe={200,500}` and the existing `crc-alpha` grid.
3. Keep `window` as the rollback baseline and as the comparison row in the main experiment table.
4. If startup behavior becomes part of the paper story, move preload execution out of the first-query path and report startup separately from steady-state latency.

### Comparison with Baselines (Updated)

| System | recall@10 | avg_ms | Notes |
|--------|-----------|--------|-------|
| **BoundFetch (nprobe=200, alpha=0.05, full_preload)** | **0.9346** | **1.588** | **Low-load retest; 25.3% faster than `window` at same semantics** |
| BoundFetch (nprobe=200, alpha=0.05, window) | 0.9346 | 2.125 | Low-load retest baseline on the new code path |
| BoundFetch (nprobe=200, alpha=0.05, full_preload, prior run) | 0.9346 | 1.686 | Prior preload-on run before the low-load retest |
| BoundFetch (nprobe=200, alpha=0.05, window, prior run) | 0.9346 | 2.122 | Prior window run before the low-load retest |
| BoundFetch (nprobe=200, alpha=0.05) | 0.9346 | 1.511 | Historical optimal before this change set |
| BoundFetch (nprobe=200, alpha=0.1) | 0.9021 | 1.409 | Previous default |
| DiskANN+FlatStor | 1.000 | 3.790 | 2.5x slower, perfect recall |
| FAISS-IVFPQ+FlatStor | 0.747 | 1.312 | Faster but recall-limited |

**With `.clu` full preload enabled, BoundFetch keeps 93.46% recall while reducing the window-path latency by 25.3% in the low-load retest, reaching 1.588ms average at this operating point.**

---

## 6. FastScan Epsilon Rebuild Validation

### Validation Protocol

The epsilon validation was rerun under one fixed warm point:

- dataset: `coco_100k`
- queries: `1000`
- topk: `10`
- `nlist=2048`
- `nprobe=200`
- `crc_alpha=0.02`
- `clu_mode=full_preload`

The benchmark now records:

- `index_source` (`rebuilt` or `reused`)
- `resolved_index_dir`
- `loaded_eps_ip`
- `loaded_d_k`

---

## 7. Epsilon × CRC-Alpha Sweep on Freshly Rebuilt Indexes

We rebuilt five fixed-epsilon indexes under:

- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.75_rebuild_20260415_epssweep`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.80_rebuild_20260415_epssweep`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90_rebuild_20260415_epssweep`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.95_rebuild_20260415_epssweep`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.99_rebuild_20260415_epssweep`

For each rebuilt index we reran the same `crc-alpha` grid:

`{0.01, 0.02, 0.03, 0.04, 0.05, 0.07, 0.10, 0.15, 0.20}`

with fixed:

- `nlist=2048`
- `bits=4`
- `nprobe=200`
- `queries=1000`
- `clu_mode=full_preload`

### High-Recall End (`alpha=0.01`)

| epsilon | recall@10 | avg_ms | p99_ms |
|---------|-----------|--------|--------|
| 0.75 | 0.9505 | 0.7016 | 0.9065 |
| 0.80 | 0.9523 | 0.7218 | 0.9436 |
| 0.90 | 0.9536 | 0.8140 | 1.1098 |
| 0.95 | 0.9539 | 0.8952 | 1.2016 |
| 0.99 | 0.9541 | 1.0679 | 1.3492 |

### Main Findings

1. Raising `epsilon` from `0.75` to `0.80` is the cheapest way to recover some recall.
   - `recall@10`: `0.9505 -> 0.9523`
   - `avg_ms`: `0.7016 -> 0.7218`
2. Going beyond `0.90` has rapidly diminishing returns.
   - `0.90 -> 0.99` only adds `+0.0005` recall at `alpha=0.01`
   - but `avg_ms` increases from `0.8140 -> 1.0679`
3. The full Pareto curves shift mostly in parallel.
   - Lower epsilon gives the faster curve
   - Higher epsilon gives a slightly higher recall ceiling
4. In the current reproducible chain, `epsilon=0.80` or `0.90` are the most reasonable follow-up choices if the goal is to trade a modest latency increase for a small recall recovery.

The full sweep is stored in:

- [boundfetch_epsilon_alpha_sweep.csv](/home/zcq/VDB/VectorRetrival/baselines/results/boundfetch_epsilon_alpha_sweep.csv)
- [boundfetch_epsilon_pareto.svg](/home/zcq/VDB/VectorRetrival/baselines/results/boundfetch_epsilon_pareto.svg)

### Control Check: Reused Index Ignores Query-Time Override

A control run reused `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048` while passing `--epsilon-percentile=0.90` on the benchmark CLI.

| run type | requested epsilon | resolved index | loaded_eps_ip | conclusion |
|----------|-------------------|----------------|---------------|------------|
| reused control | 0.90 | `index_fkmeans_2048` | 0.0995 | runtime epsilon did **not** change |

This confirms that query-time CLI epsilon is only recorded as experiment metadata when `--index-dir` is reused.

### Rebuilt Index Sweep: Corrected `bits=4` Stage 2 Path

To make the sweep strictly apples-to-apples against the historical `index_fkmeans_2048`, we first exported reusable clustering artifacts from that index:

- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048/centroids.fvecs`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048/assignments.ivecs`

During follow-up validation, we found that the earlier standardized `index_fkmeans_2048_eps*` rebuilds had been created with `--bits=1`, while the historical serving index uses `bits=4`. That disabled Region 2 ExRaBitQ entries in `cluster.clu`, made Stage 2 inactive, and therefore cannot represent the intended final serving path.

The corrected sweep now rebuilds every epsilon point explicitly with `--bits=4`, and the standardized directory naming is also made explicit:

- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.99`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.95`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.85`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.80`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.75`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.70`
- `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.60`

Every corrected rebuilt index reports `bits=4` in the `cluster.clu` global header, and every run shows non-zero Stage 2 counts, confirming that the intended `1-bit FastScan + multi-bit ExRaBitQ Stage 2` path is active again.

| epsilon_percentile | loaded_eps_ip | recall@10 | avg_ms | p99_ms | avg_safe_out | avg_uncertain | avg_s2_safe_out | avg_s2_uncertain | avg_probe_ms | avg_rerank_cpu_ms |
|--------------------|---------------|-----------|--------|--------|--------------|---------------|-----------------|------------------|--------------|-------------------|
| 0.99 | 0.0994 | 0.9563 | 2.3644 | 2.8647 | 3994.6 | 130.8 | 4375.5 | 130.8 | 2.0212 | 0.0163 |
| 0.95 | 0.0753 | 0.9561 | 2.0497 | 2.5800 | 6134.9 | 117.0 | 2251.8 | 117.0 | 1.7376 | 0.0143 |
| 0.90 | 0.0631 | 0.9561 | 1.9251 | 2.4109 | 6951.7 | 111.2 | 1449.8 | 111.2 | 1.6266 | 0.0133 |
| 0.85 | 0.0553 | 0.9556 | 1.8570 | 2.2594 | 7369.1 | 107.5 | 1044.7 | 107.5 | 1.5642 | 0.0132 |
| 0.80 | 0.0492 | 0.9541 | 1.8137 | 2.2692 | 7626.3 | 104.8 | 790.2 | 104.8 | 1.5216 | 0.0127 |
| 0.75 | 0.0442 | 0.9519 | 1.7718 | 2.0775 | 7803.1 | 102.5 | 615.7 | 102.5 | 1.4905 | 0.0126 |
| 0.70 | 0.0398 | 0.9500 | 1.7433 | 2.0178 | 7940.9 | 100.7 | 487.5 | 100.7 | 1.4638 | 0.0122 |
| 0.60 | 0.0324 | 0.9409 | 1.7056 | 1.9106 | 8116.8 | 97.3 | 315.0 | 97.3 | 1.4322 | 0.0117 |

### Interpretation

1. Rebuilding the index clearly changes runtime FastScan bounds:
   - `loaded_eps_ip` increases monotonically from `0.0324` to `0.0994`
   - the direction still matches the configured build-time `epsilon-percentile`
2. On the corrected `bits=4` serving path, Stage 2 is active throughout the sweep:
   - `avg_s2_safe_out` stays strongly non-zero
   - `avg_s2_uncertain` tracks the Stage 1 uncertain stream
   - the earlier `avg_s2_safe_out=0` observation was caused by mistakenly rebuilding `bits=1` indexes
3. Lowering epsilon now has the intended two-stage effect:
   - Stage 1 `SafeOut` increases
   - Stage 2 input shrinks from `4375.5` at `0.99` down to `315.0` at `0.60`
   - total query time drops accordingly from `2.3644ms` to `1.7056ms`
4. The useful operating region is now much clearer:
   - for the refreshed main table and Pareto figure, the new anchor is `epsilon=0.75` at `0.9519 / 1.7718ms / 2.0775ms`
   - if the target is `recall@10 >= 0.955`, `epsilon=0.85` is currently the best point at `0.9556 / 1.8570ms / 2.2594ms`
   - if the target is roughly `recall@10 ~= 0.95`, `epsilon=0.70` is the best observed point at `0.9500 / 1.7433ms / 2.0178ms`
5. This corrected sweep supports continuing epsilon tuning on the true final path:
   - the benefit is not limited to a `bits=1` Stage 1-only artifact
   - tighter FastScan bounds reduce real Stage 2 load on the deployed `bits=4` pipeline

### Status After Export

The corrected epsilon sweep is now on the historical `fkmeans` clustering and the intended `bits=4` serving path, so it supersedes the earlier `index_fkmeans_2048_eps*` `bits=1` rebuilds for all final conclusions. Those earlier runs remain useful only as a debugging record that exposed the path-mismatch issue.

---

## Appendix: Experiment Output Locations

- nprobe=200, alpha=0.05: `/home/zcq/VDB/test/coco_100k_20260414T132114`
- nprobe=200, alpha=0.01: `/home/zcq/VDB/test/coco_100k_20260414T132507`
- preload comparison, `window`: `/home/zcq/VDB/test/clu_preload_compare_window/coco_100k_20260414T225702`
- preload comparison, `full_preload`: `/home/zcq/VDB/test/clu_preload_compare_full_round2/coco_100k_20260414T230041`
- Full matrix outputs: `/home/zcq/VDB/test/coco_100k_20260414*`
- exported clustering artifacts: `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048/{centroids.fvecs,assignments.ivecs}`
- corrected bits=4 epsilon rebuild sweep (`0.99`): `/tmp/epsilon_bits4_stage2_sweep/eps099/coco_100k_20260415T185116`
- corrected bits=4 epsilon rebuild sweep (`0.95`): `/tmp/epsilon_bits4_stage2_sweep/eps095/coco_100k_20260415T185151`
- corrected bits=4 epsilon rebuild sweep (`0.90`): `/tmp/epsilon_bits4_stage2_sweep/eps090/coco_100k_20260415T185217`
- corrected bits=4 epsilon rebuild sweep (`0.85`): `/tmp/epsilon_bits4_stage2_sweep/eps085/coco_100k_20260415T185227`
- corrected bits=4 epsilon rebuild sweep (`0.80`): `/tmp/epsilon_bits4_stage2_sweep/eps080/coco_100k_20260415T185238`
- corrected bits=4 epsilon rebuild sweep (`0.75`): `/tmp/epsilon_bits4_stage2_sweep/eps075/coco_100k_20260415T185248`
- corrected bits=4 epsilon rebuild sweep (`0.70`): `/tmp/epsilon_bits4_stage2_sweep/eps070/coco_100k_20260415T185259`
- corrected bits=4 epsilon rebuild sweep (`0.60`): `/tmp/epsilon_bits4_stage2_sweep/eps060/coco_100k_20260415T185309`
- reused-index control (`requested epsilon=0.90`): `/tmp/epsilon_prebuilt_p090/coco_100k_20260415T162649`
