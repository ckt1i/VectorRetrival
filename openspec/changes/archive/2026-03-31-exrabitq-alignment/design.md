# Design: ExRaBitQ Alignment

## Overview

Four progressive steps, each independently testable:
1. **Precomputed clustering** — load faiss centroids/assignments, skip KMeans
2. **xipnorm correction** — fix distance formula, add per-vector correction factor
3. **Optimized quantization** — replace uniform bin with `fast_quantize`
4. **AVX2 acceleration** — SIMD inner product for multi-bit codes

## Step 0: Precomputed Clustering Support

### Problem
bench_rabitq_accuracy and bench_vector_search run KMeans from scratch each time. On deep1m (N=1M, K=4096), this takes minutes with our naive C++ KMeans.

### Design
Add CLI parameters `--centroids <path.fvecs>` and `--assignments <path.ivecs>` to both benchmarks. When provided, skip KMeans entirely and load the precomputed faiss results.

- Read `.fvecs` using existing `io::LoadVectors()`
- Read `.ivecs` using existing `io::LoadIvecs()`
- Validate: centroids dim matches base dim, assignments count matches N

### Files changed
- `benchmarks/bench_rabitq_accuracy.cpp` — add CLI args, conditional KMeans skip
- `benchmarks/bench_vector_search.cpp` — same

## Step 1: xipnorm Correction Factor (A1)

### Mathematical basis

Current (broken) formula:
```
ip_est = (1/sqrt(D)) * sum(q'[i] * reconstruction[code[i]])
dist = r^2 + r_q^2 - 2*r*r_q * ip_est
```

Correct formula:
```
ip_raw = sum(q'[i] * reconstruction[code[i]])    // no 1/sqrt(D)
ip_est = ip_raw * xipnorm                         // per-vector correction
dist = r^2 + r_q^2 - 2*r*r_q * ip_est
```

Where:
```
reconstruction[v] = -1.0 + (2*v + 1) / 2^M       // bin center, NO /sqrt(D)
xipnorm = 1.0 / sum(reconstruction[code[i]] * o'[i])
        = 1.0 / <o_q, o'>                          // quantization fidelity
```

### Why 1/sqrt(D) works for 1-bit but fails for M-bit

For M=1: reconstruction = {-1, +1}, ||o_q|| = sqrt(D), <o_q, o'> ~ sqrt(D)
  => xipnorm ~ 1/sqrt(D) — coincidentally matches the fixed scale.

For M>1: reconstruction values vary, ||o_q|| depends on the vector
  => xipnorm != 1/sqrt(D) — using fixed scale introduces systematic bias.

### Data structure changes

```cpp
struct RaBitQCode {
    std::vector<uint64_t> code;
    float norm;          // ||o - c||
    uint32_t sum_x;      // popcount(MSB plane)
    uint8_t bits = 1;
    float xipnorm = 0;  // NEW: 1 / <o_q, o'>  (0 for bits=1)
};
```

### Encoder changes (rabitq_encoder.cpp)

After quantization (step 5-6), add:
```
if bits > 1:
  for i in 0..D:
    recon[i] = -1.0 + (2*bin[i] + 1) / 2^M
    ip_oq_o += recon[i] * rotated[i]
  xipnorm = 1.0 / ip_oq_o
```

### Estimator changes (rabitq_estimator.cpp)

`PrepareQuery`: Remove LUT precomputation (no longer needed).

`EstimateDistanceMultiBitRaw`: Replace LUT lookup with:
```
for i in 0..D:
  v = extract_M_bits(code, i)
  recon = -1.0 + (2*v + 1) / 2^M       // same reconstruction, no /sqrt(D)
  ip_raw += q'[i] * recon

ip_est = ip_raw * xipnorm
dist = norm_oc^2 + norm_qc_sq - 2 * norm_oc * norm_qc * ip_est
```

### Storage impact

`code_entry_size` grows by `sizeof(float)` for the xipnorm field. This affects:
- `cluster_store.h` — code_entry_size() calculation
- `ivf_builder.cpp` — code packing into flat bytes
- `overlap_scheduler.cpp` — reading xipnorm from raw bytes
- `crc_calibrator.cpp` — code entry packing

**Decision**: For this change, only modify benchmarks. Production code changes are out of scope.

## Step 2: Optimized Quantization (A2)

### Algorithm: fast_quantize

Replace uniform bin quantization with:

1. Take `abs(o'[i])` for all dimensions
2. Search for optimal scaling factor `t` using priority queue:
   - Initialize: `t_start = (2^M - 1) / (3 * max(|o'|))`
   - `code_abs[i] = floor(t_start * |o'[i]|)`
   - Priority queue entries: `(next_t[i], i)` where `next_t[i] = (code_abs[i]+1) / |o'[i]|`
   - Pop smallest, increment that dimension's code, update cosine
   - Track maximum cosine and its corresponding `t`
3. Final quantize: `code_abs[i] = clamp(floor(t_opt * |o'[i]|), 0, 2^M-1)`
4. Sign restoration:
   - Positive: `code_stored[i] = code_abs[i]`
   - Negative: `code_stored[i] = (2^M - 1) - code_abs[i]`
5. Bit-plane packing unchanged (MSB = sign bit still holds with this scheme)
6. Recompute xipnorm with the new codes

### Why sign-flip preserves MSB = sign bit

```
M=2 (levels 0,1,2,3):
  positive, code_abs=2 → stored=2 (10) → MSB=1 ✓
  negative, code_abs=2 → stored=3-2=1 (01) → MSB=0 ✓
  positive, code_abs=0 → stored=0 (00) → MSB=0 ...

Wait — positive with code_abs=0 gives MSB=0, but sign is positive.
This means MSB != sign for small positive values.
```

This is a known difference from the uniform scheme. The MSB plane is **not** identical to the 1-bit sign code when using `fast_quantize`. The original author handles this by having separate short codes (1-bit, packed for FastScan) and long codes (M-bit). We need to preserve this separation:

- **MSB plane**: Computed from `sign[i] = (o'[i] >= 0)` directly (NOT from the top bit of code_stored)
- **M-bit code**: Stored separately using the fast_quantize + flip scheme

### Estimator formula unchanged

The xipnorm-corrected formula from Step 1 works correctly with fast_quantize codes. No estimator changes needed.

### Complexity

`fast_quantize` is O(D log D) per vector (priority queue). For D=96, this is negligible compared to the rotation step.

## Step 3: AVX2 Acceleration

### Target operation
```
ip_raw = sum(q'[i] * reconstruction(code[i]))
       = sum(q'[i] * (-1 + (2*code[i]+1) / 2^M))
```

For compacted codes (e.g., 4-bit: two values per byte), use AVX2:
- Load 32 bytes of packed codes → 64 4-bit values
- Unpack to 32-bit integers
- Convert to float, compute reconstruction, FMA with query

Reference: original author's `IP32_fxu4` and `IP64_fxu2` in `space.hpp`.

### Approach
- Add SIMD variants in `src/simd/` alongside existing L2/popcount functions
- Keep scalar fallback for non-AVX2 platforms
- Only accelerate the multi-bit inner product, not the popcount Stage 1

## Backward Compatibility

- M=1 path completely unchanged (xipnorm=0, uses popcount as before)
- All existing tests must pass
- Storage format: benchmarks compute xipnorm in-memory, no file format change yet
