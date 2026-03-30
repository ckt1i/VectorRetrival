# Proposal: RaBitQ Diagnostic Benchmark

## Problem

CRC early-stop works well in `bench_conann_crc` (exact L2 distances) but barely triggers in `bench_e2e --crc` (RaBitQ estimated distances) under the same alpha. The root cause is unclear — we need diagnostic data to understand:

1. **How much do RaBitQ estimates deviate from exact L2?** Specifically, how does the kth-distance estimate evolve as clusters are probed, compared to exact kth-distance?
2. **Does ConANN classification still work under RaBitQ estimates?** If RaBitQ noise causes frequent SafeOut/SafeIn misclassification, the entire ConANN+CRC pipeline is undermined.

Without this diagnostic data, we're guessing at the root cause and can't design effective fixes.

## Proposed Change

Create a single diagnostic benchmark `bench_rabitq_diagnostic.cpp` with two test phases:

### Phase 1: Distance Distribution Diagnosis
- For each query, maintain parallel exact-L2 and RaBitQ-estimate top-K heaps
- Record kth_dist from both heaps after each cluster probe step
- Output CSV: per-vector scatter (exact vs est), kth_dist convergence curves, d_min/d_max comparison, per-step top-K overlap

### Phase 2: ConANN Classification Under RaBitQ
- For each (query, vector) pair, compute both exact and estimated classifications
- Include both `Classify(dist, margin)` and `ClassifyAdaptive(dist, margin, dynamic_dk)` using est_kth as dynamic_dk
- Output CSV: confusion matrix data, false-SafeOut rates for true NNs, classification flip rates by margin magnitude

### Output & Analysis
- All raw data exported as CSV files for Python visualization
- Use conda `lab` environment for plotting (Jupyter/matplotlib)
- Run on both `coco_1k` and `coco_5k` datasets

## Scope

- New file: `benchmarks/bench_rabitq_diagnostic.cpp`
- Update: `benchmarks/CMakeLists.txt`
- New: `scripts/plot_rabitq_diagnostic.py` (analysis/plotting script)
- No changes to library code — this is purely diagnostic/observational
