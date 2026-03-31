# Tasks: AVX-512 SIMD Support

## Step 0: CMake

- [ ] **0.1** Update CMakeLists.txt AVX-512 block
  - Add `-mavx512vpopcntdq` to compiler flags
  - Add `add_compile_definitions(VDB_USE_AVX2=1)` when AVX512 is ON
  - Verify: `cmake -DVDB_USE_AVX512=ON ..` configures without error

## Step 1: Hot Path — ip_exrabitq.cpp + distance_l2.cpp

- [ ] **1.1** Add AVX-512 path to `ip_exrabitq.cpp`
  - 16 floats/iter: `_mm512_loadu_ps`, `_mm512_cvtepu8_epi32`, `_mm512_fmadd_ps`
  - Sign handling: `_mm512_mask_blend_ps` or `_mm512_maskz_sub_ps`
  - Horizontal sum: `_mm512_reduce_add_ps`
  - Scalar tail for dim % 16 != 0
- [ ] **1.2** Add AVX-512 path to `distance_l2.cpp`
  - 16 floats/iter: `_mm512_sub_ps`, `_mm512_fmadd_ps`
  - Horizontal sum: `_mm512_reduce_add_ps`
  - Scalar tail for d % 16 != 0
- [ ] **1.3** Build and run tests: test_rabitq_estimator, test_distance_l2
- [ ] **1.4** Benchmark bench_vector_search --bits 4 on deep1m, compare AVX2 vs AVX512

## Step 2: Popcount — popcount.cpp

- [ ] **2.1** Add AVX-512 path to `PopcountXor` using VPOPCNTDQ
  - `_mm512_xor_si512` + `_mm512_popcnt_epi64` + `_mm512_reduce_add_epi64`
  - 8 uint64_t/iter (vs AVX2's 4)
  - No VPSHUFB trick needed — single instruction popcount
- [ ] **2.2** Add AVX-512 path to `PopcountTotal` using VPOPCNTDQ
  - `_mm512_popcnt_epi64` + `_mm512_reduce_add_epi64`
- [ ] **2.3** Run test_popcount

## Step 3: Support Functions

- [ ] **3.1** Add AVX-512 path to `bit_unpack.cpp` (1-bit path)
  - `_mm512_srlv_epi32` with 16-element shift vector
  - Process 16 bits per iteration
- [ ] **3.2** Add AVX-512 path to `prefix_sum.cpp` (ExclusivePrefixSum)
  - `_mm512_alignr_epi32` for element-level rotation
  - 16 uint32_t/iter with cross-lane carry
- [ ] **3.3** Add AVX-512 path to `prefix_sum.cpp` (ExclusivePrefixSumMulti)
  - 16 parallel streams instead of 8
- [ ] **3.4** Add AVX-512 path to `transpose.cpp` (Transpose8xN/TransposeNx8)
  - 16×16 block transpose using `_mm512_unpacklo/hi_epi32` + `_mm512_shuffle_i32x4`
- [ ] **3.5** Run all SIMD tests: test_bit_unpack, test_prefix_sum, test_transpose

## Step 4: Final Validation

- [ ] **4.1** Full build with `cmake -DVDB_USE_AVX512=ON ..`
- [ ] **4.2** Run ALL unit tests
- [ ] **4.3** Run bench_vector_search and bench_rabitq_accuracy on deep1m, compare latency
