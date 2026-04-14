# Warm Serving Evaluation Analysis

**Date**: 2026-04-14
**System**: BoundFetch
**Dataset**: COCO 100K
**Protocol**: Warm steady-state only

## Executive Summary

This analysis covers the warm steady-state evaluation of BoundFetch on COCO 100K, including:
1. Main Pareto curve characterization
2. Mechanism attribution at optimal operating points
3. Extended crc_alpha ablation for recall improvement

**Key Finding**: Setting `crc_alpha=0.05` instead of the default `0.1` improves recall@10 from 0.9021 to 0.9346 (+3.2%) with only 7.2% latency increase, and significantly improves p99 latency from 2.493ms to 1.855ms (-25.6%).

---

## 1. Main Pareto Block Results (M1)

### BoundFetch nprobe Sweep (nlist=2048, alpha=0.1)

| nprobe | recall@10 | avg_ms | p99_ms | early_stop% |
|--------|-----------|--------|--------|-------------|
| 50 | 0.8068 | 0.743 | 1.001 | 17.5% |
| 100 | 0.8781 | 1.117 | 1.513 | 48.2% |
| 150 | 0.8984 | 1.336 | 2.000 | 82.4% |
| 200 | 0.9021 | 1.409 | 2.493 | 96.2% |
| 250 | 0.9024 | 1.397 | 2.744 | 99.5% |
| 300 | 0.9025 | 1.393 | 2.824 | 100.0% |
| 500 | 0.9025 | 1.403 | 2.773 | 100.0% |

**Observation**: Recall saturates at ~90.25% for nprobe≥200. Latency remains stable at ~1.4ms for nprobe=200-500.

### Baseline Comparison

| System | recall@10 | avg_ms | Notes |
|--------|-----------|--------|-------|
| BoundFetch (nprobe=200, alpha=0.1) | 0.9021 | 1.409 | Best BoundFetch at default alpha |
| DiskANN+FlatStor | 1.000 | 3.790 | Perfect recall but 2.7x slower |
| FAISS-IVFPQ+FlatStor | 0.747 | 1.312 | Faster but recall limited |

---

## 2. Mechanism Attribution (M2)

### Breakdown at Best Operating Point

| Metric | R010 (nprobe=200) | R011 (nprobe=300) | Analysis |
|--------|-------------------|-------------------|----------|
| recall@10 | 0.9021 | 0.9025 | +0.04% |
| avg_ms | 1.409 | 1.384 | -1.8% |
| p99_ms | 2.493 | 2.729 | +9.5% |
| **uring_submit_ms** | 0.732 (52%) | 0.717 (52%) | Dominant cost |
| **probe_ms** | 0.430 (31%) | 0.430 (31%) | Constant |
| **parse_cluster_ms** | 0.051 (3.6%) | 0.052 (3.8%) | Negligible |
| **rerank_cpu_ms** | 0.012 (0.9%) | 0.012 (0.9%) | Negligible |
| **io_wait_ms** | 0.002 (0.1%) | 0.001 (0.07%) | **Negligible** |
| submit_calls | 24.4 | 23.5 | Decreased |

### Key Conclusions

1. **io_wait is not the bottleneck**: <0.1% of total latency
2. **uring_submit dominates**: ~52% of total latency
3. **probe_ms is constant**: nprobe change doesn't affect probe cost
4. **qd/SQPOLL tuning is not the main lever**: io_wait is already fully hidden

---

## 3. Recall Improvement Ablation (M3)

### Extended crc_alpha × nprobe Matrix

| nprobe \ alpha | 0.01 | 0.02 | 0.05 | 0.08 | 0.1 | 0.15 | 0.2 |
|----------------|------|------|------|------|-----|------|-----|
| **200** | 0.9640 | 0.9615 | **0.9346** | ~0.919 | 0.9021 | 0.8180 | 0.7578 |
| | 2.251ms | 2.210ms | **1.511ms** | 1.466ms | 1.409ms | 0.797ms | 0.684ms |
| **250** | 0.9750 | 0.9661 | **0.9346** | 0.9170 | 0.9024 | 0.8180 | 0.7578 |
| | 2.781ms | 2.737ms | **1.568ms** | 1.479ms | 1.397ms | 0.807ms | 0.674ms |
| **300** | 0.9808 | 0.9719 | **0.9346** | 0.9171 | 0.9025 | 0.8180 | - |
| | 3.309ms | 3.234ms | **1.543ms** | 1.493ms | 1.384ms | 0.827ms | - |
| **500** | 0.9853 | 0.9843 | **0.9346** | 0.9171 | 0.9025 | 0.8180 | - |
| | 3.788ms | 5.519ms | **1.536ms** | 1.519ms | 1.403ms | 0.816ms | - |

*Format: recall@10 / avg_ms*

### Key Findings

1. **alpha=0.05 achieves optimal recall-latency tradeoff**:
   - recall@10 = 0.9346 (vs 0.9021 at alpha=0.1, +3.2%)
   - avg_ms = 1.511 (vs 1.409, +7.2%)
   - **p99_ms = 1.855 (vs 2.493, -25.6% improvement!)**

2. **alpha sensitivity is smooth and predictable**:
   - Smaller alpha → more clusters explored → higher recall, higher latency
   - The tradeoff is monotonic and controllable

3. **epsilon-percentile has no effect at nprobe≥200**:
   - Already fully exploring clusters; boundary tightens don't help

4. **alpha=0.01 achieves highest recall (~0.98) but at significant latency cost**

### Optimization Gate: PASSED

**Decision**: Continue code optimization with alpha=0.05 as the new default.

**Rationale**:
- Significant recall improvement (+3.2%) at acceptable latency (+7.2%)
- Dramatic p99 improvement (-25.6%) improves tail latency
- Smooth parameter space allows further tuning if needed

---

## 4. Conclusions and Recommendations

### Optimal Configurations

| Scenario | nprobe | alpha | recall@10 | avg_ms | p99_ms |
|----------|--------|-------|-----------|--------|--------|
| **Best Tradeoff** | 200 | 0.05 | 0.9346 | 1.511 | 1.855 |
| **Lowest Latency** | 200 | 0.15 | 0.8180 | 0.797 | 1.032 |
| **Highest Recall** | 500 | 0.01 | 0.9853 | 3.788 | 4.313 |

### Next Steps

1. **Use alpha=0.05 as the new default** for BoundFetch
2. **Consider nprobe sweep at alpha=0.05** to find if higher nprobe improves recall further
3. **Proceed to Block C writeup** — recall improvement gate passed
4. **Consider opening a new change** for further recall optimization if needed

### Comparison with Baselines (Updated)

| System | recall@10 | avg_ms | Notes |
|--------|-----------|--------|-------|
| **BoundFetch (nprobe=200, alpha=0.05)** | **0.9346** | **1.511** | **New optimal** |
| BoundFetch (nprobe=200, alpha=0.1) | 0.9021 | 1.409 | Previous default |
| DiskANN+FlatStor | 1.000 | 3.790 | 2.5x slower, perfect recall |
| FAISS-IVFPQ+FlatStor | 0.747 | 1.312 | Faster but recall-limited |

**BoundFetch with alpha=0.05 achieves 93.46% recall at 1.5ms, a strong tradeoff against DiskANN's 100% recall at 3.79ms.**

---

## Appendix: Experiment Output Locations

- nprobe=200, alpha=0.05: `/home/zcq/VDB/test/coco_100k_20260414T132114`
- nprobe=200, alpha=0.01: `/home/zcq/VDB/test/coco_100k_20260414T132507`
- Full matrix outputs: `/home/zcq/VDB/test/coco_100k_20260414*`
