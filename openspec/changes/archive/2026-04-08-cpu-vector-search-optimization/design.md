# Design: CPU-Level Vector Search Optimization

## Architecture overview

```
Query (96-dim float32)
    │
    ▼
┌─────────────────────────────────────┐
│ MKL cblas_sgemv (centroid scan)     │  ← Phase 1 done (2%)
│ + std::sort top-nprobe              │  ← Phase 2: pre-alloc (8%)
└───────────────┬─────────────────────┘
                │ top-100 clusters
                ▼
┌─────────────────────────────────────┐
│ Per-Cluster Probe Loop              │
│ PrepareQuery ← Phase 2: reuse buf  │  ← (6%)
│                                     │
│  ┌───────────────────────────────┐  │
│  │ Stage 1: FastScan (VPSHUFB)  │  │  ← Phase 1 done (5%)
│  │ batch-32, 86% SafeOut        │  │
│  └──────────┬────────────────────┘  │
│       SafeOut│    Uncertain (14%)   │
│          ✗   │        ▼             │
│              │  ┌──────────────┐    │
│              │  │ Stage 2 ★★★ │    │  ← Phase 2: prefetch + SOA (44%)
│              │  │ IPExRaBitQ   │    │
│              │  │ (memory-     │    │
│              │  │  bound!)     │    │
│              │  └──────┬───────┘    │
│              │         ▼            │
│              │  Rerank L2Sqr        │  ← (inlined by LTO)
│              │  Heap update         │  ← Phase 2: pre-alloc (5%)
│  └──────────┴────────────────────┘  │
│  CRC Early Stop                     │
└─────────────────────────────────────┘
```

## Design decisions

### D1: SOA layout for ex_code/ex_sign

**Current** (AOS with heap indirection):
```
codes[vid].ex_code → std::vector<uint8_t> (heap-allocated, scattered)
codes[vid].ex_sign → std::vector<uint8_t> (heap-allocated, scattered)
```

**Proposed** (SOA, flat contiguous):
```
struct RaBitQCodeSOA {
    std::vector<uint8_t> all_ex_code;  // N * dim bytes, contiguous
    std::vector<uint8_t> all_ex_sign;  // N * dim bytes, contiguous
    std::vector<float>   all_norms;    // N floats
    std::vector<float>   all_xipnorm;  // N floats
    Dim dim;

    const uint8_t* ex_code(uint32_t vid) const {
        return all_ex_code.data() + static_cast<size_t>(vid) * dim;
    }
    const uint8_t* ex_sign(uint32_t vid) const {
        return all_ex_sign.data() + static_cast<size_t>(vid) * dim;
    }
};
```

**Impact**: Eliminates 2M heap allocations (1M vectors x 2 vectors). Sequential vid access now triggers hardware prefetcher. Total data footprint: 96MB + 96MB = 192MB contiguous (vs scattered).

**Scope**: Apply only to the benchmark (`bench_vector_search.cpp`). The core `RaBitQCode` struct remains unchanged -- SOA is an alternative representation for query-time use.

### D2: Software prefetch for Stage 2

Insert `_mm_prefetch` calls in the FastScan classification loop. When a vector is classified as S1-Uncertain, prefetch the *next* uncertain vector's code data.

```
Challenge: We don't know which vectors will be uncertain in advance.

Solution: Prefetch speculatively for ALL vectors in the block,
          since prefetch is a hint (no penalty if unused).
          
          Alternative: Track uncertain vid list, prefetch ahead in that list.
```

Prefetch distance: 3-5 vectors ahead (DRAM latency ~200 cycles, IPExRaBitQ ~40 cycles per vector).

Prefetch targets:
- `codes_soa.ex_code(vid)` -- 96 bytes = 2 cache lines
- `codes_soa.ex_sign(vid)` -- 96 bytes = 2 cache lines
- Use `_MM_HINT_T0` (prefetch to L1)

### D3: PrepareQuery buffer reuse

Add a `PrepareQueryInto` method that writes into an existing `PreparedQuery` object, reusing its allocated vectors:

```cpp
void PrepareQueryInto(const float* query, const float* centroid,
                      const RotationMatrix& rotation,
                      PreparedQuery* pq) const;
```

The method calls `resize()` instead of allocating new vectors. After the first call, subsequent calls reuse the existing capacity with zero allocations.

### D4: Per-query temporary pre-allocation

Move these allocations outside the query loop in `bench_vector_search.cpp`:
- `centroid_dists` (4096 pairs, 32KB)
- `est_heap`, `exact_heap` (reserve capacity)
- `qc` vector for MKL sgemv result

## Risk analysis

| Risk | Mitigation |
|------|------------|
| SOA layout diverges from core RaBitQCode | Keep RaBitQCode unchanged; SOA is a parallel view for benchmark/query |
| Prefetch may not help if S2 calls are too sparse | Measure prefetch hit rate with `perf stat -e L1-dcache-prefetch-misses` |
| PrepareQueryInto changes estimator API | Non-breaking: add new method alongside existing PrepareQuery |
| Recall regression from code changes | All changes are performance-only, no algorithmic changes |
