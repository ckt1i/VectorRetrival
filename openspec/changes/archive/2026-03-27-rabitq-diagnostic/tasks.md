# Tasks: RaBitQ Diagnostic Benchmark

## 1. Build Infrastructure

- [x] 1.1 Add `bench_rabitq_diagnostic` target in `benchmarks/CMakeLists.txt` (link: `vdb_index vdb_io`)
- [x] 1.2 Create `benchmarks/bench_rabitq_diagnostic.cpp` with CLI arg parsing and Phase 0 (data load, KMeans, encode, calibrate ε_ip/d_k, compute GT)

## 2. Phase 1: Distance Distribution Diagnosis

- [x] 2.1 Implement dual-heap probing loop: for each query, probe clusters in centroid order, maintain parallel exact_heap and est_heap, record kth_dist and top-K overlap at each step
- [x] 2.2 Write `kth_convergence.csv`: (query_id, probed_count, exact_kth, est_kth, topk_overlap)
- [x] 2.3 Write `vector_distances.csv`: per-vector (query_id, vector_id, cluster_id, exact_dist, est_dist, is_true_topk) for first `scatter_queries` queries
- [x] 2.4 Compute and print terminal summary: d_min/d_max in exact vs est space, mean kth gap per step, final mean overlap

## 3. Phase 2: ConANN Classification Under RaBitQ

- [x] 3.1 Implement classification loop: for each (query, cluster, vector), compute margin, Classify with exact/est, ClassifyAdaptive with est+est_kth, record is_true_topk
- [x] 3.2 Write `classification.csv`: (query_id, vector_id, cluster_id, probe_step, exact_dist, est_dist, margin, class_exact, class_est, class_est_adaptive, is_true_topk)
- [x] 3.3 Compute and print terminal summary: confusion matrices, false SafeOut rates, classification flip rate

## 4. Python Plotting Script

- [x] 4.1 Create `scripts/plot_rabitq_diagnostic.py` with argparse (--dir for CSV input directory)
- [x] 4.2 Plot 1: scatter (est_dist vs exact_dist) colored by is_true_topk
- [x] 4.3 Plot 2: kth convergence curves (mean ± P25/P75 bands for exact_kth and est_kth)
- [x] 4.4 Plot 3: classification confusion heatmaps (exact vs est, exact vs est_adaptive)
- [x] 4.5 Plot 4: false SafeOut rate by margin quartile
- [x] 4.6 Plot 5: top-K overlap curve (mean overlap vs probed_count)

## 5. Validation

- [x] 5.1 Build and run on coco_1k: `bench_rabitq_diagnostic --dataset /home/zcq/VDB/data/coco_1k --outdir ./diag_output/coco_1k`
- [x] 5.2 Build and run on coco_5k: `bench_rabitq_diagnostic --dataset /home/zcq/VDB/data/coco_5k --outdir ./diag_output/coco_5k`
- [x] 5.3 Run plotting script and verify figures: `conda run -n lab python scripts/plot_rabitq_diagnostic.py --dir ./diag_output/coco_1k`
