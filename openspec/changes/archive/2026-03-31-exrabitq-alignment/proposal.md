# Proposal: ExRaBitQ Alignment

## Summary

Align our multi-bit RaBitQ implementation with the Extended-RaBitQ (ExRaBitQ) paper's algorithm to fix the recall collapse on large datasets (deep1m recall@10 = 0.15 vs. original author's 0.95).

## Problem

Our current multi-bit RaBitQ has three fundamental algorithmic flaws compared to the original ExRaBitQ:

1. **Missing xipnorm correction factor**: We use a fixed `1/sqrt(D)` scale for the inner product estimate. The correct approach stores a per-vector correction factor `xipnorm = 1 / <o_q, o'>` that compensates for quantization distortion. This is the **primary cause** of recall collapse on large datasets.

2. **Suboptimal quantization**: We use uniform bin quantization (`floor((v+1)/2 * 2^M)`). The original uses an optimized `fast_quantize` that searches for the scaling factor `t` maximizing `<o_q, o'> / ||o_q||` (cosine similarity between quantized and original vector).

3. **Slow benchmark iteration**: KMeans runs from scratch every benchmark invocation. Need to support loading precomputed faiss centroids/assignments for fast iteration.

## Baseline Evidence

Original author's Extended-RaBitQ on deep1m (K=4096):
- 4-bit: recall@100 = 0.935 at nprobe=100, 0.950 at nprobe=1000
- 3-bit: recall@100 = 0.894 at nprobe=100, 0.906 at nprobe=1000

Our implementation on coco100k (bits=8):
- recall@10 = 0.145, final_safeout_pct = 97.89%

## Scope

### In scope
- Precomputed centroid/assignment loading in benchmarks
- xipnorm correction factor (encoder + estimator + storage)
- Optimized `fast_quantize` algorithm
- AVX2 SIMD acceleration for the multi-bit inner product
- Unit tests and benchmark validation on deep1m

### Out of scope
- Production code changes (overlap_scheduler, ivf_builder) — benchmark-only first
- AVX-512 FastScan block-32 batch processing (original author's optimization)
- Changes to the 1-bit (M=1) path — must remain backward compatible

## Success Criteria

- deep1m recall@100 >= 0.90 at nprobe=100 with bits=4 in bench_vector_search
- All existing unit tests pass unchanged
- bench_rabitq_accuracy shows distance ratio < 1.01 for bits=4
