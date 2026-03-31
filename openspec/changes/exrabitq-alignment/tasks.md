# Tasks: ExRaBitQ Alignment

## Step 0: Precomputed Clustering Support

- [x] **0.1** Add `--centroids` and `--assignments` CLI params to `bench_rabitq_accuracy.cpp`
  - When provided, load `.fvecs` / `.ivecs` and skip KMeans
  - Validate dimensions match
- [x] **0.2** Add same params to `bench_vector_search.cpp`
- [x] **0.3** Verify: run both benchmarks on deep1m with precomputed faiss results
  - `--centroids data/deep1m/deep1m_centroid_4096.fvecs --assignments data/deep1m/deep1m_cluster_id_4096.ivecs`
  - Confirm results match (same recall as running with built-in KMeans on smaller dataset)

## Step 1: xipnorm Correction Factor

- [x] **1.1** Add `float xipnorm` field to `RaBitQCode` struct in `rabitq_encoder.h`
  - Default value 0.0f (unused for bits=1)
- [x] **1.2** Compute xipnorm in `RaBitQEncoder::Encode()` for bits > 1
  - After quantization: `xipnorm = 1.0 / sum(recon[i] * rotated[i])`
  - Guard against division by zero (if sum == 0, set xipnorm = 1.0)
- [x] **1.3** Modify `RaBitQEstimator::EstimateDistanceMultiBitRaw()`
  - Remove LUT usage
  - Compute `ip_raw = sum(q'[i] * (-1 + (2*code[i]+1)/2^M))` directly
  - Apply correction: `ip_est = ip_raw * xipnorm`
  - Distance formula unchanged: `dist = r^2 + r_q^2 - 2*r*r_q * ip_est`
- [x] **1.4** Remove LUT from `PreparedQuery` (no longer needed for estimation)
  - Removed `lut` field from PreparedQuery, removed LUT precomputation from PrepareQuery()
- [x] **1.5** Add unit tests for xipnorm
  - `Xipnorm_IsFiniteAndPositive`: bits=2,4 — verified
  - `Xipnorm_Bits1_IsZero`: bits=1 xipnorm=0 — verified
  - `Stage2_Xipnorm_CloserToExact`: S2 error < S1 error with bits=4 — verified
- [x] **1.6** Run bench_rabitq_accuracy on deep1m --bits 4 with precomputed centroids
  - S2 MAE: 0.123 → 0.043 (↓65%), S2 MRE: 0.069 → 0.023 (↓66%)
  - Compare recall and distance error before/after xipnorm
  - Expected: significant recall improvement

## Step 2: Optimized Quantization (fast_quantize)

- [x] **2.1** Implement `FastQuantize()` in `rabitq_encoder.cpp`
  - Priority queue search for optimal scaling factor t
  - Stores ex_code (per-dim code_abs) + ex_sign + xipnorm in RaBitQCode
  - Bit-plane layout preserved for Stage 1 (MSB = sign via uniform quantization)
- [x] **2.2** Modify `RaBitQEncoder::Encode()` for bits > 1
  - M=1: unchanged sign quantization
  - M>1: uniform bit-plane for Stage 1 + fast_quantize ex_code for Stage 2
- [x] **2.3** Update estimator to use ex_code/ex_sign for Stage 2
  - `EstimateDistanceMultiBit()` uses ex_code path when available
  - Bit-plane Raw path kept as fallback
- [x] **2.4** Run bench_rabitq_accuracy on deep1m --bits 4
  - **Result: Stage 2 MAE = 0.006 (from 0.123, 20x improvement)**
  - Stage 2 MRE = 0.003, P99 = 0.012
  - Unit tests: 38/38 pass

## Step 3: AVX2 SIMD Acceleration

- [x] **3.1** Add `simd::IPExRaBitQ()` in `src/simd/ip_exrabitq.cpp`
  - Input: float query[D], uint8_t code_abs[D], uint8_t sign[D], D
  - AVX2 implementation (8 floats/iteration with FMA) + scalar fallback
  - Works directly with ex_code/ex_sign arrays (no packing needed)
- [x] **3.2** Integrate SIMD into `EstimateDistanceMultiBit()`
  - Changed ex_sign from `vector<bool>` to `vector<uint8_t>` for SIMD compat
  - EstimateDistanceMultiBit calls `simd::IPExRaBitQ()` when ex_code available
- [x] **3.3** Benchmark QPS improvement on deep1m
  - bits=4, nprobe=50, 100 queries: **4.66ms → 2.90ms (38% faster)**
  - recall unchanged at 0.973, 0 False SafeOut
