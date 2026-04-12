# Pipeline Summary

**Problem**: Disk-based vector search wastes 90%+ I/O bandwidth on non-Top-K candidates when co-retrieving payloads, or adds a full I/O round-trip for sequential payload fetch.
**Final Method Thesis**: Use RaBitQ error bounds for three-class I/O scheduling — SafeOut pruning (90-98%), SafeIn prefetch (activation conditions under exploration), Uncertain on-demand verification — via single-thread io_uring overlap on co-designed dual-file layout.
**Final Verdict**: READY (conditional: SafeIn activation needs experimental validation)
**Date**: 2026-04-10 (v3)

## Final Deliverables
- Proposal: `refine-logs/FINAL_PROPOSAL.md` / `FINAL_PROPOSAL_CN.md` (v3)
- Review summary: `refine-logs/REVIEW_SUMMARY.md` / `REVIEW_SUMMARY_CN.md` (v3)
- Refinement report: `refine-logs/REFINEMENT_REPORT.md` / `REFINEMENT_REPORT_CN.md` (v3)
- Experiment plan: `refine-logs/EXPERIMENT_PLAN.md` / `EXPERIMENT_PLAN_CN.md` (v3)
- Experiment tracker: `refine-logs/EXPERIMENT_TRACKER.md` / `EXPERIMENT_TRACKER_CN.md` (v3)

## Contribution Snapshot
- Dominant contribution: **SafeOut pruning** — eliminates 90-98% of candidates via error bounds
- Exploratory contribution: **SafeIn prefetch** — theoretically complete, activation conditions under exploration
- Supporting contributions: Dual-file layout, single-thread io_uring overlap, FastScan→ExRaBitQ pipeline
- Explicitly rejected: Graph index, multi-threading, GPU, distributed, filtered search

## Must-Prove Claims
- C1: SafeOut reduces I/O 40-60% vs eager-fetch
- C2: Single-thread overlap >80% CPU-I/O ratio
- C3: Co-located layout 2-3x better than separated
- C4: ExRaBitQ Stage 2 tightens Uncertain rate
- C5: CRC early-stop 30-73% (top_k dependent)
- C6: search+payload end-to-end beats SPANN/FAISS/DiskANN
- C7: SafeIn ≥5% activation under specific conditions (exploratory)

## Preliminary Key Findings
```
SafeOut rate:    90-98% (Stage 1, varies with top_k)
SafeIn rate:     ≈ 0% (all current configs — exploration needed)
CRC early-stop:  top_k=10 → 73%, top_k=20 → 21% (inverse relationship)
top_k tradeoff:  SafeOut↑ vs early-stop↓ (interesting analysis point)
```

## First Runs to Launch
1. **E6-quick**: Deep1M top_k={50,100} to extend SafeOut/early-stop trend
2. **E3-quick**: Deep1M classification ablation (full vs SafeOut-only vs no-class vs eager)
3. **E8-quick**: Deep1M bits={1,2,4,8} × top_k={10,50,100} SafeIn rate scan

## Main Risks
- **SafeIn stays at 0%**: SafeOut pruning alone is still a strong contribution; SafeIn as future work
- **CRC early-stop top_k sensitivity**: Report honestly, analyze complementary relationship
- **Graph index pure-search advantage**: Don't compare pure search; focus search+payload end-to-end
- **Real dataset preparation time**: Use existing Deep1M/COCO for fast validation first

## Next Action
- Run E6-quick and E3-quick (existing Deep1M data, <half day)
- Start preparing MS MARCO Passage dataset
- Proceed to `/run-experiment`
