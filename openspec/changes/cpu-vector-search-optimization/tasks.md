# Tasks: CPU-Level Vector Search Optimization

## Phase 1 (Completed)

- [x] Update CMakeLists.txt compiler flags (`-march=icelake-server`, `-flto`, `-funroll-loops`, `-fno-math-errno`)
- [x] Add `VDB_USE_MKL` option to CMakeLists.txt with sequential lp64 linking
- [x] Integrate `cblas_sgemv` into `IvfIndex::FindNearestClusters` with precomputed centroid norms
- [x] Add MKL-accelerated centroid scan to `bench_vector_search.cpp`
- [x] Rebuild and validate: 5.1ms -> 2.3ms, recall@10 = 0.99

## Phase 2

### Task 1: Pre-allocate per-query temporaries in benchmark

- [x] Done

**Files**: `benchmarks/bench_vector_search.cpp`

### Task 2: Add `PrepareQueryInto` to RaBitQEstimator

- [x] Done

**Files**: `include/vdb/rabitq/rabitq_estimator.h`, `src/rabitq/rabitq_estimator.cpp`, `benchmarks/bench_vector_search.cpp`

### Task 3: SOA code layout for Stage 2

- [x] Done

**Files**: `benchmarks/bench_vector_search.cpp`

### Task 4: Software prefetch for Stage 2 data

- [x] Done

**Files**: `benchmarks/bench_vector_search.cpp`

### Task 5: Benchmark and profile

- [x] Done

Results (5 runs, CRC enabled, MKL_NUM_THREADS=1):
- latency avg (stable): 2.2-2.3ms (vs Phase 1: 2.6ms with CRC)
- recall@10 = 0.9650 (with CRC), 0.9900 (without CRC)
- No regression in classification counts

Target of 1.5ms not yet reached — further optimization needed (deferred batch S2, etc.)
