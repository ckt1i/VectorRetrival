# Tasks: E2E Two-Stage Alignment

## Step 1: OverlapScheduler Stage 2 (DONE)

- [x] **1.1** Add S2 stats to SearchStats
- [x] **1.2** Add `#include "vdb/simd/ip_exrabitq.h"` to overlap_scheduler.cpp
- [x] **1.3** Modify `ProbeCluster()` to add S2 logic after S1 classification
- [x] **1.4** Add `bits()` accessor / cache `margin_s2_divisor` in OverlapScheduler
- [x] **1.5** Fix RaBitQEncoder to pass `config_.rabitq.bits` in IvfBuilder

## Step 2: eps_ip Calibration (REVISED — always popcount)

- [x] **2.1** CalibrateEpsilonIpFastScan() added to ivf_builder.cpp (unused, kept as reference)
- [x] **2.2** Reverted: always use popcount CalibrateEpsilonIp() regardless of bits

## Step 3: bench_e2e Simplification (DONE)

- [x] **3.1** Remove dual-round logic, always CRC
- [x] **3.2** Update output format with S2 stats
- [x] **3.3** Ensure bits propagation through build → index → query

## Step 4: False SafeIn/Out Detection (DONE)

- [x] **4.1** Add false_safeout/false_safein_upper to QueryResult/RoundMetrics
- [x] **4.2** Add False SafeOut/SafeIn reporting to console + JSON
- [x] **4.3** Build and verify compilation

## Step 5: Phase A — Popcount est_heap for CRC

- [x] **5.1** Add popcount distance computation to ProbeCluster est_heap
  - Add `#include "vdb/simd/popcount.h"` and `#include "vdb/storage/pack_codes.h"`
  - For each vector in each FastScan block, compute popcount distance:
    - `UnpackSignBitsFromFastScan(block_ptr, j, dim, code_words)`
    - `hamming = PopcountXor(pq.sign_code.data(), code_words, num_words)`
    - `ip_pop = 1 - 2*hamming/dim`
    - `dist_pop = norm_oc² + norm_qc² - 2*norm_oc*norm_qc*ip_pop`
  - Push `dist_pop` (not `dists[j]`) into `est_heap_`
  - Allocate `code_words` buffer (num_words uint64_t) once per ProbeCluster call
- [x] **5.2** Revert S2 margin to design formula: `margin_s2 = margin / margin_s2_divisor_`
  - Remove the temporary `s2_d_k = dynamic_d_k + margin` hack
  - S2 ClassifyAdaptive uses `dynamic_d_k` (now popcount-based, safe)
- [x] **5.3** Build, run tests, verify compilation
- [ ] **5.4** Run bench_e2e on coco_1k bits=4 — quick sanity check
  - Expect: recall@10 ≥ 0.99, false_safeout ≈ 0
- [ ] **5.5** Run bench_e2e on coco_100k bits=4 --queries 100
  - Expect: recall@10 ≥ 0.99, false_safeout ≈ 0
  - Compare S1/S2 stats with bench_vector_search reference
- [ ] **5.6** Run bench_e2e on coco_100k bits=1 --queries 100 (regression)
  - S2 should be skipped entirely
  - Expect: recall@10 ≥ 0.95

## Step 6: Phase B — Fallback to S1 popcount (only if Phase A fails)

- [ ] **6.1** If 5.4/5.5 show false SafeOut > 0:
  - Replace FastScan S1 classification with popcount-based EstimateDistanceRaw
  - Use UnpackSignBitsFromFastScan + EstimateDistanceRaw for each vector
  - This makes S1 fully popcount-aligned with bench_vector_search
- [ ] **6.2** Re-run validation tests
