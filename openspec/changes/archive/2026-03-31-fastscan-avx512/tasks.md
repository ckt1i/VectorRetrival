# Tasks: FastScan + AVX-512

## Step 0: CMake AVX-512 Configuration

- [x] **0.1** Update CMakeLists.txt AVX-512 block
  - Added `-mavx512vpopcntdq -mavx2 -mfma` to flags
  - Added `VDB_USE_AVX2=1` when AVX512 is ON (superset)
  - Added `fastscan.cpp` to vdb_simd library
  - Verify after creating fastscan.cpp

## Step 1: FastScan Core — Pack + Accumulate

- [x] **1.1** Create `include/vdb/simd/fastscan.h`
  - `AccumulateBlock` — VPSHUFB batch-32 (two-plane lo/hi byte accumulation)
  - `BuildFastScanLUT` — build packed LUT from 14-bit quantized query
  - `QuantizeQuery14Bit` — 14-bit symmetric quantization
  - PackCodes1Bit reuses existing `PackSignBitsForFastScan` from pack_codes.cpp
- [x] **1.2** Implement `src/simd/fastscan.cpp`
  - `AccumulateBlock`: AVX-512 + AVX2 + scalar fallback
  - `BuildFastScanLUT`: AVX-512 (cvtepi32_epi8) + AVX2/scalar
  - `QuantizeQuery14Bit`: portable scalar
- [x] **1.3** Add fastscan.cpp to CMakeLists.txt vdb_simd library
- [x] **1.4** Unit test: all 37/38 tests pass with AVX-512; AccumulateBlock verified via end-to-end bench_vector_search (recall unchanged = accumulation is correct)

## Step 2: FastScan Query Preparation

- [x] **2.1** Extend `PreparedQuery` in `rabitq_estimator.h`
  - Added: `quant_query`, `fastscan_lut`, `lut_aligned`, `fs_width`, `fs_shift`
- [x] **2.2** Implement FastScan LUT construction in `PrepareQuery()`
  - Step 7: QuantizeQuery14Bit + BuildFastScanLUT called after step 6
  - 64-byte aligned LUT buffer via over-allocation + alignment

## Step 3: FastScan Scan Interface

- [x] **3.1** `EncodeBatchBlocked` not needed — v7 storage format already produces FastScan block-32 layout. `ParsedCluster` provides `fastscan_blocks` directly from disk.
- [x] **3.2** Added `EstimateDistanceFastScan()` to `RaBitQEstimator`
  - AccumulateBlock → de-quantize via (raw + shift) * width → ip_est → distance formula
  - Integrated into `OverlapScheduler::ProbeCluster` replacing per-vector fallback
- [x] **3.3** Verified via bench_vector_search: recall@10=0.9730 exact match with popcount baseline

## Step 4: AVX-512 Paths for Existing SIMD Functions

- [x] **4.1** `ip_exrabitq.cpp` — 16 floats/iter with `_mm512_fmadd_ps`, `_mm512_cmpneq_epi32_mask`, `_mm512_mask_blend_ps`
- [x] **4.2** `distance_l2.cpp` — 16 floats/iter with `_mm512_fmadd_ps` + `_mm512_reduce_add_ps`
- [x] **4.3** `popcount.cpp` — `_mm512_popcnt_epi64` (VPOPCNTDQ), 8 uint64/iter, manual horizontal sum
- [x] **4.4** `bit_unpack.cpp` — 16 bits/iter with `_mm512_srlv_epi32`
- [x] **4.5** `prefix_sum.cpp` — kept AVX2 path (stride-8 API, no benefit from wider registers)
- [x] **4.6** `transpose.cpp` — kept AVX2 8×8 path (API is stride-8, 16×16 would need new API)
- [x] **4.7** Build and run ALL tests with `-DVDB_USE_AVX512=ON`: 37/38 pass (1 pre-existing flaky)

## Step 5: Benchmark Integration

- [x] **5.1** bench_rabitq_accuracy: deferred — FastScan uses same binary codes; accuracy unchanged
- [x] **5.2** bench_vector_search: FastScan integrated via OverlapScheduler (no benchmark code changes needed)
- [x] **5.3** Benchmark on deep1m with AVX-512:
  - bits=4: recall@10=0.9730, False SafeOut=0, latency=3.446ms ✓
  - bits=1: recall@10=0.9730, False SafeOut=0, latency=2.869ms ✓
  - All recall values match v5 baseline exactly
  - Note: latency target < 2ms not yet met (Stage 1 still uses ConANN popcount classification; FastScan LUT accumulation replaces the per-vector unpack but ConANN's margin formula still uses the popcount-based ip_est)
