# Review Summary

**Date**: 2026-04-10 (v3: updated with preliminary experiments)

---

## Round 1: Method Focus

### Strengths
1. Clear problem anchor: vector search + payload co-retrieval I/O gap is real and understudied
2. Principled mechanism: RaBitQ closed-form error bounds drive I/O decisions
3. Implementation maturity: 530+ tests, 15 development phases, production-grade SIMD
4. Natural ablation structure: each component independently disableable

### Resolved Weaknesses
| Weakness | Resolution |
|----------|-----------|
| Too broad framing | Narrowed to bound-driven I/O scheduling for search+payload |
| Too many equal contributions | Hierarchy: SafeOut pruning > layout, overlap, multi-stage |
| "Bag of tricks" perception | Unified under: error bounds → confidence → I/O scheduling |
| Missing graph index positioning | Analyzed why graphs don't suit payload co-prefetch (§5.1) |

---

## Round 2: Preliminary Experiment Corrections

**Finding 1: SafeIn ≈ 0%**
- All configs (COCO 768-dim, Deep1M 96-dim, top_k=10/20): SafeIn ≈ 0%
- **Handling**: Retain in framework for completeness + exploratory claim C7

**Finding 2: SafeOut-CRC inverse relationship**
```
Deep1M: top_k=10 → SafeOut 93.79%, early_stop 100%, probed 27/100
        top_k=20 → SafeOut 97.91%, early_stop 21%,  probed 85.64/100
```
- **Handling**: Systematically vary top_k, analyze as interesting tradeoff

**Finding 3: Stage 2 ExRaBitQ contribution**
- Meaningful but not transformative additional SafeOut on Uncertain candidates

---

## Round 3: Anticipated Reviewer Concerns

| Concern | Pre-emptive Answer |
|---------|-------------------|
| "SafeIn never fires, three-class is misleading?" | Report honestly; focus narrative on SafeOut; SafeIn as exploratory contribution with systematic condition search |
| "Why not compare pure search with DiskANN?" | Our contribution is search+payload; graph pointer-chasing doesn't suit payload co-prefetch |
| "CRC fails at large top_k?" | Report top_k sensitivity honestly; note SafeOut compensates |
| "Single thread realistic?" | Focus on per-query latency; throughput via query-level parallelism |
| "Synthetic datasets?" | Primary datasets are real: MS MARCO, COCO, Amazon Products |

---

## Final Verdict

**READY (conditional)** — SafeOut pruning is validated (90-98%), CRC early-stop is validated with interesting top_k sensitivity, SafeIn activation conditions need experimental exploration. Proceed to experiments.
