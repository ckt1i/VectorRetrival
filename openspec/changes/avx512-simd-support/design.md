# Design: AVX-512 SIMD Support

## Architecture

3-tier compile-time dispatch in every SIMD file:

```cpp
#if defined(VDB_USE_AVX512)
  // AVX-512 path (16 floats / 8 uint64_t per iteration)
#elif defined(VDB_USE_AVX2)
  // AVX2 path (8 floats / 4 uint64_t per iteration)
#else
  // Scalar fallback
#endif
```

## CMake Change

```cmake
if(VDB_USE_AVX512)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx512vpopcntdq -mavx2 -mfma")
    add_compile_definitions(VDB_USE_AVX512=1)
    add_compile_definitions(VDB_USE_AVX2=1)  # superset: AVX2 code still available
elseif(VDB_USE_AVX2)
    # unchanged
```

Key: `VDB_USE_AVX512` implies `VDB_USE_AVX2`. Files that only have AVX2 paths will still work.

## Per-File Design

### 1. ip_exrabitq.cpp (★★★★★ priority)

Current AVX2: 8 floats/iter with `_mm256_fmadd_ps`

AVX-512 upgrade:
- `_mm512_loadu_ps` — load 16 query floats
- `_mm_loadu_si128` — load 16 uint8_t code_abs
- `_mm512_cvtepu8_epi32` — expand 16 uint8 → 16 int32
- `_mm512_cvtepi32_ps` + `_mm512_add_ps(half)` — code_abs + 0.5
- Sign: `_mm512_cvtepu8_epi32` + `_mm512_slli_epi32(31)` → mask → `_mm512_mask_blend_ps`
- `_mm512_fmadd_ps` — FMA
- `_mm512_reduce_add_ps` — horizontal sum (built-in, no manual folding!)

### 2. distance_l2.cpp (★★★★ priority)

Current AVX2: 8 floats/iter

AVX-512 upgrade:
- `_mm512_loadu_ps` — load 16 floats
- `_mm512_sub_ps` — diff
- `_mm512_fmadd_ps` — sum += diff*diff
- `_mm512_reduce_add_ps` — horizontal sum

### 3. popcount.cpp (★★★ priority)

Current AVX2: VPSHUFB nibble-lookup, 4 uint64_t/iter

AVX-512 upgrade with VPOPCNTDQ:
- `_mm512_loadu_si512` — load 8 uint64_t
- `_mm512_xor_si512` — XOR
- `_mm512_popcnt_epi64` — **direct 64-bit popcount** (no VPSHUFB trick needed!)
- `_mm512_reduce_add_epi64` — horizontal sum

This is a **major simplification** — the entire VPSHUFB + SAD + batch overflow logic is replaced by one instruction.

### 4. bit_unpack.cpp (★★ priority)

Current AVX2: 8 uint32_t/iter for 1-bit unpack

AVX-512 upgrade:
- `_mm512_set1_epi32(byte)` — broadcast to 16 lanes
- `_mm512_srlv_epi32` — variable shift (16 different shift amounts)
- `_mm512_and_si512` — mask to 0/1
- Process 16 bits per iteration instead of 8

### 5. prefix_sum.cpp (★★ priority)

Current AVX2: 8 uint32_t/iter with lane-crossing carry

AVX-512 upgrade:
- `_mm512_loadu_si512` — load 16 uint32_t
- Intra-512 prefix sum using `_mm512_alignr_epi32` (element-level rotation, not byte-level!)
- Cross-lane carry via `_mm512_mask_add_epi32` or scalar extraction
- Multi-stream: 16 parallel streams instead of 8

### 6. transpose.cpp (★ priority)

Current AVX2: 8×8 block transpose using unpack + permute (24 instructions)

AVX-512 upgrade: 16×16 block is significantly more complex.
- Use `_mm512_unpacklo_epi32` / `_mm512_unpackhi_epi32` (32-bit interleave)
- `_mm512_shuffle_i32x4` — 128-bit lane permutation
- 4 phases instead of 3, more instructions but 4x more data per block
- Scalar tail for num_streams < 16

## Testing Strategy

- Rebuild with `-DVDB_USE_AVX512=ON`
- Run all existing SIMD unit tests (test_distance_l2, test_popcount, test_bit_unpack, test_prefix_sum, test_transpose)
- Run rabitq unit tests (test_rabitq_encoder, test_rabitq_estimator)
- Benchmark: bench_vector_search with bits=1 and bits=4
