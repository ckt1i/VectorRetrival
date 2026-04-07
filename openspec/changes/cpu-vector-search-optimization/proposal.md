# CPU-Level Vector Search Optimization

## Summary

Optimize the vector search pipeline (IVF + RaBitQ + ConANN) at the CPU/memory level to reduce single-query latency on Intel Xeon Silver 4310 (Ice Lake-SP, AVX-512). Target: halve search latency while maintaining recall@10 >= 0.95.

## Background

Benchmark baseline on deep1M dataset (dim=96, nlist=4096, nprobe=100, bits=4, 200 queries):

| Metric | Original | After Phase 1 | Target |
|--------|----------|---------------|--------|
| latency avg | 5.1 ms | 2.3 ms | ~1.2 ms |
| latency p50 | 5.1 ms | 2.1 ms | ~1.0 ms |
| recall@10 | 0.99 | 0.99 | >= 0.95 |

### Phase 1 (Completed)

Three changes delivered a **55% latency reduction** (5.1ms -> 2.3ms):

1. **Compiler flags** -- `-march=icelake-server -mtune=icelake-server -flto -funroll-loops -fno-math-errno` replaced manual `-mavx512*` flags. LTO enables cross-TU inlining of hot SIMD functions (L2Sqr, AccumulateBlock).

2. **MKL integration** -- `cblas_sgemv` for centroid distance computation. Reduced centroid scan from ~30% to ~3% of search time. Centroid norms precomputed at index open.

3. **FastScan enablement** -- VPSHUFB batch-32 Stage 1 classification now active. Stage 1 SafeOut rate improved from 67% to 86%, reducing Stage 2 + rerank workload by ~60%.

### Phase 1 profiling findings

After Phase 1, perf profile shows the bottleneck has shifted:

| Function | % of search | Root cause |
|----------|-------------|------------|
| EstimateDistanceMultiBit (Stage 2) | ~44% | Random DRAM access to `codes[vid].ex_code/ex_sign` |
| main (inline: L2Sqr rerank, classify) | ~23% | L2Sqr rerank + ConANN classification |
| centroid sort | ~8% | `std::sort` on 4096 pairs |
| PrepareQuery | ~6% | Per-cluster: FWHT + LUT build + heap allocs |
| EstimateDistanceFastScan | ~5% | Already fast (AVX-512 VPSHUFB) |
| heap operations | ~5% | `push_heap`/`pop_heap` on est/exact heaps |
| alloc/free | ~4% | Per-query `std::vector` allocation (centroid_dists, heaps) |
| RotationMatrix::Apply | ~3% | Hadamard FWHT O(n log n) |
| MKL sgemv | ~2% | Nearly free |

**Key insight**: Stage 2 is memory-bound, not compute-bound. `IPExRaBitQ` takes ~15ns of computation but ~100ns of DRAM latency per vector. The `ex_code`/`ex_sign` data (96+96 bytes per vector) is stored in per-vector `std::vector` objects scattered across the heap.

## Scope

### Phase 2 optimizations (this proposal)

1. **Prefetch Stage 2 data** -- Software prefetch `codes[vid].ex_code/ex_sign` for upcoming S1-Uncertain vectors while processing current vector.

2. **SOA code layout** -- Replace per-vector `std::vector<uint8_t> ex_code/ex_sign` with flat contiguous arrays `flat_ex_code[N * dim]`, `flat_ex_sign[N * dim]`. Eliminates pointer indirection and enables hardware prefetcher.

3. **Reuse PrepareQuery allocations** -- Pre-allocate `PreparedQuery` buffers once per query instead of per-cluster. Eliminates ~344 malloc/free calls per query.

4. **Pre-allocate per-query temporaries** -- Move `centroid_dists`, `est_heap`, `exact_heap` allocations outside the query loop.

### Out of scope

- Multi-threaded search (stays single-threaded per decision)
- Algorithmic changes to ConANN thresholds or nprobe tuning
- Deferred batch Stage 2 (high complexity, consider in Phase 3)
- GEMM-based batch query processing (throughput optimization, not latency)

## Success criteria

- latency avg <= 1.5 ms on deep1M (nprobe=100, bits=4)
- recall@10 >= 0.95
- No regression in existing tests
