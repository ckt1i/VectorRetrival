# Proposal: AVX-512 SIMD Support

## Summary

Add AVX-512 implementations for all 6 SIMD source files, doubling throughput for compute-intensive operations. The CPU supports full AVX-512 including VPOPCNTDQ.

## Problem

All SIMD functions only have AVX2 paths (256-bit, 8 floats/iter). The CPU supports AVX-512 (512-bit, 16 floats/iter) which can theoretically 2x throughput. The CMake option `VDB_USE_AVX512` exists but no source file implements the `#ifdef VDB_USE_AVX512` path.

## Scope

### In scope
- CMake: fix AVX512 to also define VDB_USE_AVX2 (superset)
- `ip_exrabitq.cpp` — ExRaBitQ Stage 2 inner product (hot path)
- `distance_l2.cpp` — L2 squared distance (rerank hot path)
- `popcount.cpp` — PopcountXor/PopcountTotal using `_mm512_popcnt_epi64` (VPOPCNTDQ)
- `bit_unpack.cpp` — 1-bit unpack path
- `prefix_sum.cpp` — exclusive prefix sum (single + multi-stream)
- `transpose.cpp` — 8×8 → 16×16 block transpose

### Out of scope
- Runtime dispatch (compile-time only via `#ifdef`)
- Changing public API signatures

## Success Criteria

- All existing unit tests pass with `-DVDB_USE_AVX512=ON`
- bench_vector_search bits=4 latency measurably reduced vs AVX2
