# Expected Test Results — RaBitQ Storage v7 Validation

Reference baseline: exrabitq-alignment change (v5 format, commit before storage v7).

## Test Commands

```bash
cd /home/zcq/VDB/VectorRetrival/build && cmake .. && cmake --build .

# 1. Unit tests
./test_rabitq_encoder && ./test_rabitq_estimator

# 2. bench_rabitq_accuracy (bits=4, deep1m)
./benchmarks/bench_rabitq_accuracy \
  --base /home/zcq/VDB/data/deep1m/deep1m_base.fvecs \
  --query /home/zcq/VDB/data/deep1m/deep1m_query.fvecs \
  --centroids /home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --bits 4 --queries 10 --topk 10

# 3. bench_vector_search (bits=4, deep1m)
./benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/deep1m/deep1m_base.fvecs \
  --query /home/zcq/VDB/data/deep1m/deep1m_query.fvecs \
  --gt /home/zcq/VDB/data/deep1m/deep1m_groundtruth.ivecs \
  --centroids /home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --nlist 4096 --nprobe 50 --topk 10 --bits 4 --queries 100 --early-stop 0

# 4. bench_vector_search (bits=1, regression check)
./benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/deep1m/deep1m_base.fvecs \
  --query /home/zcq/VDB/data/deep1m/deep1m_query.fvecs \
  --gt /home/zcq/VDB/data/deep1m/deep1m_groundtruth.ivecs \
  --centroids /home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --nlist 4096 --nprobe 50 --topk 10 --bits 1 --queries 100 --early-stop 0
```

---

## 1. Unit Tests

| Test suite | Expected |
|---|---|
| test_rabitq_encoder | 18/18 PASSED |
| test_rabitq_estimator | 20/20 PASSED |

---

## 2. bench_rabitq_accuracy (bits=4, deep1m, 10 queries)

### v5 Baseline (measured)

| Metric | Stage 1 (popcount) | Stage 2 (fast_quantize) |
|---|---|---|
| Mean absolute error | 0.105398 | 0.005921 |
| Mean relative error | 0.057465 | 0.003208 |
| P95 relative error | 0.148114 | 0.008322 |
| P99 relative error | 0.211283 | 0.011557 |

| Ranking | Stage 1 | Stage 2 |
|---|---|---|
| recall@1 | 1.0000 | 1.0000 |
| recall@5 | 0.5800 | 1.0000 |
| recall@10 | 0.4000 | 0.9600 |

### v7 Expected

**Must match v5 baseline exactly** (storage format change only, no algorithm change).

- Stage 1 MAE: ~0.105
- Stage 2 MAE: ~0.006
- Stage 2 recall@10: ~0.96

### Pass criteria
- Stage 2 MAE < 0.01
- Stage 2 recall@10 >= 0.90
- Stage 1 values unchanged from v5

---

## 3. bench_vector_search (bits=4, deep1m, 100 queries, nprobe=50)

### v5 Baseline (measured)

```
recall@1  = 1.0000
recall@5  = 0.9800
recall@10 = 0.9730

False SafeOut = 0 / 1277554 (0.0000%)

Stage 1 SafeOut:   77.67%
Stage 1 Uncertain: 22.31%

Stage 2 (input = S1 Uncertain):
  SafeIn:    0.19%
  SafeOut:  94.99%
  Uncertain: 4.82%

Final SafeOut:     98.86%
Final Uncertain:    1.07%

latency avg = 2.904 ms
```

### v7 Expected

| Metric | Expected | Tolerance |
|---|---|---|
| recall@1 | 1.0000 | exact |
| recall@5 | 0.9800 | exact |
| recall@10 | 0.9730 | exact |
| False SafeOut | 0 | **must be 0** |
| Final SafeOut % | ~98.86% | ±1% |
| Final Uncertain % | ~1.07% | ±1% |
| latency avg | ~2.9ms | ±50% ok (storage overhead may vary) |

### Pass criteria
- recall@10 = 0.9730 (exact match with v5)
- False SafeOut = 0
- No crash or segfault

---

## 4. bench_vector_search (bits=1, regression check)

### v5 Baseline (measured)

```
recall@1  = 1.0000
recall@5  = 0.9800
recall@10 = 0.9730

False SafeOut = 0
Stage 1 SafeOut: 78.40%
latency avg = 2.668 ms
```

### v7 Expected

Must match v5 baseline exactly. bits=1 has no Region 2 (no ExRaBitQ codes).

---

## Troubleshooting Guide

| Symptom | Likely Cause |
|---|---|
| recall drops slightly (0.97 → 0.96) | Sign bit packing/unpacking off-by-one in pack_codes |
| recall drops severely (< 0.5) | pack_codes nibble permutation wrong, or block padding error |
| False SafeOut > 0 | norm_oc read offset wrong in Region 1 factors |
| S2 MAE >> 0.01 | ex_code/ex_sign/xipnorm offset calculation wrong in Region 2 |
| Crash / segfault | Block size or region offset mismatch, buffer overrun |
| bits=1 recall drops | Region 1 packing broke 1-bit path |
| "version mismatch" error | kFileVersion not bumped, or lookup entry size mismatch |

---

## Original Author Baseline (Extended-RaBitQ, for reference)

deep1m, K=4096, 4-bit, recall@100 (not recall@10):

| nprobe | recall@100 | QPS |
|---|---|---|
| 50 | 0.907 | 3913 |
| 100 | 0.935 | 2797 |
| 1000 | 0.950 | 653 |

Note: Original author uses recall@100 with TOPK=100, not recall@10 with TOPK=10.
Our benchmarks use TOPK=10 by default.
