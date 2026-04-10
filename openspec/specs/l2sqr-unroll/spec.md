## ADDED Requirements

### Requirement: L2Sqr AVX-512 4x unrolled implementation
The AVX-512 path of `L2Sqr` SHALL use 4 independent `__m512` accumulator registers (`sum0..sum3`), processing 64 floats per iteration. The implementation SHALL be placed in `include/vdb/simd/distance_l2.h` as a header-only inline function. The `src/simd/distance_l2.cpp` file SHALL only contain AVX2 and scalar fallback paths.

#### Scenario: Correctness for dim=512
- **WHEN** L2Sqr computes squared L2 distance between two 512-dim vectors
- **THEN** result matches scalar implementation exactly (bit-identical, same FMAD rounding)

#### Scenario: Correctness for non-multiple-of-64 dimensions
- **WHEN** dim is not a multiple of 64 (e.g., dim=96)
- **THEN** scalar tail loop handles remaining elements after the 4x-unrolled main loop

#### Scenario: Non-AVX-512 builds
- **WHEN** compiled without AVX-512
- **THEN** AVX2 or scalar path in distance_l2.cpp is used (no regression)

### Requirement: Header-only SIMD function placement
All SIMD-accelerated functions introduced by this change SHALL be implemented in `include/vdb/simd/` header files, using `VDB_FORCE_INLINE` and conditional compilation (`#if defined(VDB_USE_AVX512)`).

#### Scenario: Existing tests pass
- **WHEN** the existing L2Sqr test suite runs
- **THEN** all tests pass with the new 4x-unrolled implementation
