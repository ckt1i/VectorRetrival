# Proposal: FastScan + AVX-512 SIMD

## Summary

Two-part upgrade: (1) Replace per-vector popcount Stage 1 with batch-32 VPSHUFB FastScan for higher accuracy and throughput, (2) Add AVX-512 paths for all SIMD functions. CPU supports full AVX-512 including VPOPCNTDQ.

## Problem

Current Stage 1 has two limitations:
1. **Per-vector serial**: PopcountXor called once per vector, no cross-vector parallelism
2. **1-bit query precision**: Query sign-quantized to 1 bit → high error (MAE=0.105) → 22% Uncertain needing Stage 2

FastScan (from original Extended-RaBitQ) processes 32 vectors simultaneously with 14-bit query precision.

## Baseline (deep1m, bits=4, nprobe=50, AVX2)

- Stage 1 MAE: 0.105, SafeOut: 78%, Uncertain: 22%
- Stage 2 MAE: 0.006, final SafeOut: 99%
- Latency: 2.9ms/query

## Scope

### In scope
- **Part A: FastScan** — block-32 packing, LUT construction, VPSHUFB accumulation, benchmark integration
- **Part B: AVX-512** — all 6 SIMD files (ip_exrabitq, distance_l2, popcount with VPOPCNTDQ, bit_unpack, prefix_sum, transpose)
- CMake AVX-512 configuration

### Out of scope
- Production code (overlap_scheduler, ivf_builder)
- Runtime ISA dispatch (compile-time `#ifdef` only)

## Success Criteria

- All unit tests pass with `-DVDB_USE_AVX512=ON`
- Stage 1 precision improves (fewer Uncertain)
- bench_vector_search latency < 2ms/query on deep1m
