# Design: FastScan + AVX-512

## Overview

Two parts: (A) FastScan for Stage 1 batch processing, (B) AVX-512 for all SIMD.

## Part A: FastScan Stage 1

### Concept

Replace per-vector `PopcountXor → ip` with batch-32 `VPSHUFB LUT → 32 ips`.

```
Current:                              FastScan:
for vec in cluster:                   for block of 32 vecs:
  hamming = PopcountXor(q, code)        result[32] = VPSHUFB(LUT, codes)
  ip = 1 - 2*hamming/D                 // 32 ips in ~D/4 VPSHUFB ops
  // 1 vec at a time                   // 32 vecs simultaneously
```

### Data Structures

**Block-32 packed codes** (new):
```cpp
struct PackedBlock {
    // 1-bit codes packed for VPSHUFB: D/4 groups × 32 bytes per group
    // Within each 32 bytes: low nibble = vec[0..15], high nibble = vec[16..31]
    // Each nibble encodes 4 consecutive sign bits
    std::vector<uint8_t> packed_codes;  // size = D/4 * 32 (aligned to 64)

    // Per-vector factors (32 entries)
    float norm_oc[32];    // ||o - c||
    float xipnorm[32];    // ExRaBitQ correction (0 for bits=1)
    uint32_t vec_ids[32]; // original vector IDs

    // ExRaBitQ per-vector codes (Stage 2, only populated for bits > 1)
    uint8_t* ex_codes;    // [32][D] code_abs values
    uint8_t* ex_signs;    // [32][D] sign values
    uint32_t count;       // actual vectors in this block (≤32, last block may be partial)
};
```

### Query Preparation

For FastScan, query preprocessing differs from current:

```
Current PrepareQuery:
  1. residual = q - c
  2. normalize → q̄
  3. rotate → q̄'
  4. sign_code = pack_sign_bits(q̄')     ← 1-bit, used for popcount
  5. sumq = Σ q̄'[i]

FastScan PrepareQuery (additional):
  6. Scalar quantize q̄' to int16 (14-bit range):
     vl = min(q̄'), vr = max(q̄')
     width = (vr - vl) / (2^14 - 1)
     quant_q[d] = round((q̄'[d] - vl) / width)

  7. Build LUT[D/4][16]:
     For each group of 4 dims (m = 0..D/4-1):
       for nibble = 0..15:
         LUT[m][nibble] = Σ_{k=0}^{3} quant_q[m*4+k] × bit(nibble, 3-k)

  8. Pack LUT for SIMD:
     Split into upper/lower 8-bit halves
     Arrange for VPSHUFB lane structure
```

### Encoding: pack_codes

Transform per-vector sign bits into block-32 VPSHUFB layout:

```
Input: 32 vectors' 1-bit codes (each: ceil(D/64) uint64_t)

For each group of 4 dims (m = 0..D/4-1):
  For each pair of vectors (j = 0..15):
    low_nibble  = sign[j][m*4+0]<<3 | sign[j][m*4+1]<<2 | sign[j][m*4+2]<<1 | sign[j][m*4+3]
    high_nibble = sign[j+16][m*4+0]<<3 | ... same for vec j+16
    packed[m*16 + perm[j]] = low_nibble | (high_nibble << 4)
```

The permutation `perm` ensures VPSHUFB lane alignment (matches original author's pack_codes).

### Scan Loop

```
for each block:
  // VPSHUFB accumulation (D/4 iterations, each processes 32 vecs)
  for m = 0..D/4-1:
    codes_32 = load(packed_codes + m*32)
    lo = codes_32 & 0x0F
    hi = codes_32 >> 4
    accu[0..15]  += VPSHUFB(LUT[m], lo)
    accu[16..31] += VPSHUFB(LUT[m], hi)

  // De-quantize: ip[v] = accu[v] * delta + corrections
  // Distance: dist[v] = norm[v]² + norm_qc² - 2*norm[v]*norm_qc*ip[v]/√D
  // Classify: SafeIn/Out/Uncertain for all 32

  // Stage 2 for Uncertain (per-vector, using ex_codes)
  mask = uncertain_mask
  while (mask):
    j = ctz(mask)
    dist_s2 = IPExRaBitQ(query, ex_codes[j], ex_signs[j]) * xipnorm[j]
    ...
```

## Part B: AVX-512 Upgrade

3-tier compile-time dispatch:

```cpp
#if defined(VDB_USE_AVX512)
  // 512-bit path
#elif defined(VDB_USE_AVX2)
  // 256-bit path
#else
  // Scalar
#endif
```

### CMake

```cmake
if(VDB_USE_AVX512)
    set(CMAKE_CXX_FLAGS "... -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx512vpopcntdq -mavx2 -mfma")
    add_compile_definitions(VDB_USE_AVX512=1 VDB_USE_AVX2=1)
```

### Per-file AVX-512 changes

| File | Key change | Intrinsics |
|------|-----------|------------|
| ip_exrabitq.cpp | 16 floats/iter | `_mm512_fmadd_ps`, `_mm512_reduce_add_ps` |
| distance_l2.cpp | 16 floats/iter | `_mm512_fmadd_ps`, `_mm512_reduce_add_ps` |
| popcount.cpp | VPOPCNTDQ | `_mm512_popcnt_epi64` (replaces VPSHUFB trick) |
| bit_unpack.cpp | 16 bits/iter | `_mm512_srlv_epi32` |
| prefix_sum.cpp | 16 elements | `_mm512_alignr_epi32` |
| transpose.cpp | 16×16 blocks | `_mm512_shuffle_i32x4` |

### FastScan uses AVX-512 natively

`accumulate_block` uses `_mm512_shuffle_epi8` (AVX-512BW). The original author already has AVX-512 FastScan code that we reference directly.

## File Map

```
New files:
  include/vdb/simd/fastscan.h          ← PackedBlock, accumulate, pack_codes
  src/simd/fastscan.cpp                ← VPSHUFB accumulation (AVX2 + AVX512)

Modified files:
  CMakeLists.txt                       ← AVX-512 flags, add fastscan.cpp
  include/vdb/rabitq/rabitq_encoder.h  ← EncodeBatchBlocked()
  src/rabitq/rabitq_encoder.cpp        ← block-32 packing logic
  include/vdb/rabitq/rabitq_estimator.h ← ScanBlock() batch interface
  src/rabitq/rabitq_estimator.cpp      ← FastScan query prep + scan loop
  src/simd/ip_exrabitq.cpp             ← add AVX-512 path
  src/simd/distance_l2.cpp             ← add AVX-512 path
  src/simd/popcount.cpp                ← add VPOPCNTDQ path
  src/simd/bit_unpack.cpp              ← add AVX-512 path
  src/simd/prefix_sum.cpp              ← add AVX-512 path
  src/simd/transpose.cpp               ← add AVX-512 path
  benchmarks/bench_rabitq_accuracy.cpp ← use blocked scan
  benchmarks/bench_vector_search.cpp   ← use blocked scan
```
