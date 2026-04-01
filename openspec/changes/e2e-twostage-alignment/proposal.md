# Proposal: E2E Two-Stage Alignment

## Summary

Align the production query path (OverlapScheduler + bench_e2e) with bench_vector_search:

1. **OverlapScheduler Stage 2**: After FastScan S1, Uncertain vectors get ExRaBitQ S2 re-classification.
2. **bench_e2e simplification**: Remove dual CRC/non-CRC comparison rounds. Always use CRC.
3. **eps_ip calibration**: Keep popcount-calibrated eps_ip (conservative, safe for FastScan). FastScan calibration was attempted but produces margins too narrow for CRC dynamic d_k.
4. **Popcount est_heap**: CRC estimate heap must use popcount distances (not FastScan). FastScan est_heap gives a tighter dynamic_d_k that causes false SafeOuts on large datasets.
5. **False SafeIn/Out detection**: bench_e2e reports false SafeOut/SafeIn for safety verification.

## Problem

### Core Issue Discovered: FastScan CRC est_heap Bias

The OverlapScheduler uses CRC with a dynamic d_k from an estimate heap. The heap is populated with FastScan distance estimates. FastScan (14-bit quantization) produces tighter estimates than popcount (1-bit), causing the est_heap's k-th distance to converge LOWER than popcount would give.

`ClassifyAdaptive` uses `d_k_eff = min(d_k, dynamic_d_k)`. When `dynamic_d_k` drops below the safe threshold, the SafeOut zone expands and GT vectors are incorrectly pruned.

```
N=1k:   FastScan est_kth ≈ popcount est_kth → recall=0.996 ✓
N=100k: FastScan est_kth << popcount est_kth → recall=0.074 ✗
```

bench_vector_search doesn't have this problem because it uses popcount distances for the est_heap — the natural noise of popcount keeps est_kth conservatively high.

### Fix Strategy (Two Phases)

**Phase A (popcount est_heap)**: Change OverlapScheduler to compute popcount distances for est_heap (unpack sign bits from FastScan blocks, PopcountXor). Keep FastScan for S1 classification. This matches bench_vector_search's approach.

**Phase B (fallback — only if Phase A insufficient)**: If Phase A reveals that mixing FastScan dist with popcount d_k causes issues, fall back to using popcount for S1 classification entirely.

## Scope

### In scope
- OverlapScheduler::ProbeCluster — add Stage 2 ExRaBitQ for Uncertain vectors
- OverlapScheduler::ProbeCluster — popcount est_heap for CRC dynamic d_k
- eps_ip calibration — always use popcount calibration (conservative)
- bench_e2e — simplify to CRC-only, add False SafeOut detection
- Cross-benchmark validation on COCO 100k

### Out of scope
- bench_vector_search code changes
- Storage format changes
- FastScan algorithm changes

## Success Criteria

- bench_e2e recall@10 matches bench_vector_search on COCO 100k (bits=4, ±0.005)
- 0 False SafeOut in bench_e2e (with nprobe=nlist, early_stop=0)
- OverlapScheduler Uncertain% drops from ~22% to ~1% (with bits=4)
- All existing tests pass
