# Design: RaBitQ Diagnostic Benchmark

## Overview

A single benchmark binary `bench_rabitq_diagnostic` that runs two diagnostic phases, outputting CSV data for Python visualization. Reuses the data loading / KMeans / RaBitQ encoding pattern from `bench_rabitq_accuracy.cpp`.

## Architecture

```
bench_rabitq_diagnostic
  │
  ├── Phase 0: Data load + KMeans + RaBitQ encode + calibrate ε_ip, d_k
  │     (identical to bench_rabitq_accuracy Phase 1–3)
  │
  ├── Phase 1: Distance Distribution Diagnosis
  │     → CSV: vector_distances.csv, kth_convergence.csv
  │
  ├── Phase 2: ConANN Classification Under RaBitQ
  │     → CSV: classification.csv
  │
  └── Terminal summary (key statistics)
```

## Phase 0: Shared Setup

Reuse the bench_rabitq_accuracy pattern:
1. Load `image_embeddings.npy`, `query_embeddings.npy`
2. KMeans clustering → centroids, assignments, cluster_members
3. RaBitQ encode all vectors → `codes[N]`
4. RotationMatrix (seeded)
5. Calibrate per-cluster `r_max`, global `ε_ip` (P95 IP error), `d_k` (from query set)
6. Compute brute-force exact GT: for each query, top-K exact L2 nearest IDs

## Phase 1: Distance Distribution Diagnosis

For each query `q` (all Q queries):
1. Sort clusters by centroid distance (exact L2 to centroid)
2. Maintain two parallel max-heaps of size `top_k`:
   - `exact_heap`: keyed by `L2(q, v)`
   - `est_heap`: keyed by `RaBitQ.EstimateDistance(pq, code)`
3. After probing each cluster `p` (0..nlist-1), record:
   - `exact_kth`: heap top of exact_heap (or +inf if not full)
   - `est_kth`: heap top of est_heap (or +inf if not full)
   - `overlap`: |exact_topK ∩ est_topK| (set intersection of current top-K IDs)

### CSV Output: `vector_distances.csv`

Per-vector scatter data (sampled — first `sample_queries` queries, all vectors per query):

```
query_id,vector_id,cluster_id,exact_dist,est_dist,is_true_topk
0,42,3,1.234,1.456,1
0,17,3,5.678,5.234,0
...
```

### CSV Output: `kth_convergence.csv`

Per-query per-step convergence:

```
query_id,probed_count,exact_kth,est_kth,topk_overlap
0,1,inf,inf,0
0,2,4.56,5.23,3
0,3,3.21,4.89,5
...
```

This is the **most critical output** — directly shows whether est_kth converges slower than exact_kth.

### Terminal Summary

- d_min/d_max in exact space vs est space (computed from kth values across all queries × all steps)
- Mean/P50/P95 of (est_kth - exact_kth) at each probe step
- Mean top-K overlap at final step

## Phase 2: ConANN Classification Under RaBitQ

For each query `q`, for each cluster `c` (sorted by centroid distance), for each vector `v` in cluster `c`:

1. Compute:
   - `exact_dist = L2(q, v)`
   - `est_dist = RaBitQ.Estimate(q, code_v)` (already computed in Phase 1 — cache)
   - `margin = 2 · r_max[c] · norm_qc · ε_ip`
   - `class_exact = Classify(exact_dist, margin)` using static d_k
   - `class_est = Classify(est_dist, margin)` using static d_k
   - `class_est_adaptive = ClassifyAdaptive(est_dist, margin, est_kth)` where est_kth is the current est_heap top after probing up to this cluster
   - `is_true_topk`: whether v is in the exact GT top-K

### CSV Output: `classification.csv`

```
query_id,vector_id,cluster_id,probe_step,exact_dist,est_dist,margin,class_exact,class_est,class_est_adaptive,is_true_topk
0,42,3,2,1.234,1.456,0.5,SafeIn,SafeIn,Uncertain,1
...
```

### Terminal Summary

- Confusion matrix: exact class vs est class (3×3)
- Confusion matrix: exact class vs est_adaptive class (3×3)
- False SafeOut rate (true NN classified as SafeOut) for each method
- Classification flip rate overall and by margin quartile
- SafeOut% comparison: exact vs est vs est_adaptive

## Decision Record

| ID | Decision | Rationale |
|----|----------|-----------|
| D1 | Single binary, two phases | Shared data setup; run both or either via `--phase` flag |
| D2 | CSV output for Python plotting | User has conda `lab` env; CSV is simple and universal |
| D3 | Cache per-vector distances across phases | Phase 1 already computes all exact + est distances; Phase 2 reuses them |
| D4 | Sample vector_distances.csv but full kth_convergence.csv | Vector scatter can be huge (Q×N rows); kth convergence is Q×nlist rows (manageable) |
| D5 | Include ClassifyAdaptive with est_kth | This is exactly the online path in OverlapScheduler — must test |

## CLI Parameters

```
--dataset     /path/to/data    (default: /home/zcq/VDB/data/coco_1k)
--nlist       32               (number of clusters)
--queries     100              (number of queries, 0 = all)
--topk        10               (top-K for heaps)
--outdir      ./diag_output    (CSV output directory)
--phase       0                (0=both, 1=dist only, 2=classify only)
--p-for-dk    90               (percentile for d_k)
--p-for-eps   95               (percentile for ε_ip)
--scatter-queries 10           (queries to include in vector_distances.csv)
```

## Python Plotting Script

`scripts/plot_rabitq_diagnostic.py` — reads CSVs, produces:

1. **Scatter plot**: est_dist vs exact_dist, colored by is_true_topk
2. **kth convergence curves**: x=probed_count, y=mean kth_dist, two lines (exact/est), with shaded P25–P75 bands
3. **d_min/d_max comparison bar chart**
4. **Classification confusion heatmaps** (exact vs est, exact vs est_adaptive)
5. **False SafeOut rate by margin quartile**
6. **Top-K overlap curve**: x=probed_count, y=mean overlap

Uses matplotlib + pandas. Invoked as:
```bash
conda run -n lab python scripts/plot_rabitq_diagnostic.py --dir ./diag_output
```
