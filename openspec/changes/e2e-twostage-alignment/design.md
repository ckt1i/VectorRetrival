# Design: E2E Two-Stage Alignment

## Part 1: OverlapScheduler Stage 2 (DONE)

S2 logic added to ProbeCluster for S1-Uncertain vectors when bits > 1. Uses IPExRaBitQ from Region 2 data. margin_s2 = margin_s1 / 2^(bits-1). S2 stats tracked in SearchStats.

## Part 2: bench_e2e Simplification (DONE)

Removed dual-round logic. CRC always enabled. Single search round with CRC early stop.

## Part 3: eps_ip Calibration (REVISED)

**Decision**: Always use popcount-calibrated eps_ip. FastScan calibration was implemented and tested but produces margins too narrow — when combined with CRC dynamic d_k, it causes massive false SafeOuts. Popcount eps_ip is conservative (overestimates margin), which is safe. S2 efficiently handles the extra Uncertain vectors.

CalibrateEpsilonIpFastScan() remains in ivf_builder.cpp but is not called. The build always calls CalibrateEpsilonIp() (popcount path).

## Part 4: Popcount est_heap for CRC (NEW — Phase A)

### Problem

The CRC estimate heap in OverlapScheduler uses FastScan distances. FastScan is more precise than popcount, causing est_kth to converge lower → dynamic_d_k drops → SafeOut threshold drops → false SafeOuts.

```
ClassifyAdaptive(dist, margin, dynamic_d_k):
  d_k_eff = min(d_k, dynamic_d_k)
  SafeOut: dist > d_k_eff + 2*margin

With FastScan est_heap:
  dynamic_d_k = FastScan est_kth ≈ 1.2  (tight)
  SafeOut threshold = 1.2 + 2*0.13 = 1.46 → too many SafeOuts

With popcount est_heap:
  dynamic_d_k = popcount est_kth ≈ 1.5  (conservative)
  SafeOut threshold = min(1.36, 1.5) + 2*0.13 = 1.62 → safe
```

### Solution

For each block of 32 vectors, compute popcount distances alongside FastScan:

```
for each FastScan block:
  dists[32] = EstimateDistanceFastScan(pq, block, norms)   // S1 classification

  // Popcount distances for est_heap only
  for j in 0..31:
    UnpackSignBitsFromFastScan(block, j, dim) → code_words[num_words]
    hamming = PopcountXor(pq.sign_code, code_words, num_words)
    ip_pop = 1 - 2*hamming/dim
    dist_pop = norm² + norm_qc² - 2*norm*norm_qc*ip_pop
    est_heap.push(dist_pop)       // CRC heap uses popcount

  // S1 classification uses FastScan dists[]
  for j in 0..31:
    ClassifyAdaptive(dists[j], margin, dynamic_d_k)   // d_k from popcount heap
```

### Key Properties

1. **S1 classification** uses FastScan dist (more accurate) + popcount-based dynamic_d_k (conservative)
2. **S2 classification** uses ExRaBitQ dist + popcount-based dynamic_d_k
3. **est_heap** only contains popcount distances → matches bench_vector_search
4. **CRC early stop** uses popcount est_kth → conservative stopping

### Performance

Per-vector overhead for popcount:
- `UnpackSignBitsFromFastScan`: ~D/4 byte reads + nibble extraction
- `PopcountXor`: D/64 = 8 uint64 words × popcnt instruction
- Distance formula: 4 float ops

For D=512: negligible compared to FastScan VPSHUFB cost.

### Phase B Fallback (only if Phase A insufficient)

If Phase A reveals that mixing FastScan dist with popcount d_k causes recall issues:
- Replace `EstimateDistanceFastScan` with per-vector popcount `EstimateDistanceRaw` for S1 classification
- This makes S1 fully popcount (same as bench_vector_search)
- FastScan would only be used for S2 or removed entirely

Phase B is a last resort — it would lose FastScan's 14-bit precision advantage.

## Part 5: False SafeIn/SafeOut Detection (DONE)

bench_e2e computes false_safeout by comparing predicted_ids against brute-force GT. false_safein_upper estimated as safe_in - hits.

## Part 6: Cross-Benchmark Validation

After popcount est_heap fix, run on COCO 100k:
- bench_vector_search: --crc 0 --early-stop 0 --queries 100 (reference, recall=1.0)
- bench_e2e: --early-stop 0 --queries 100 (target, should match)

## Files Modified

```
src/query/overlap_scheduler.cpp   — ProbeCluster: S2 logic + popcount est_heap
src/query/overlap_scheduler.h     — has_s2_, margin_s2_divisor_ fields
include/vdb/query/search_context.h — S2 stats in SearchStats
src/index/ivf_builder.cpp         — always use popcount eps_ip calibration
benchmarks/bench_e2e.cpp          — CRC-only, false SafeOut detection
```
