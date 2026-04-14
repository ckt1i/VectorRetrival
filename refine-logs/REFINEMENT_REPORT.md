# Refinement Report

**Problem**: Re-anchor BoundFetch after synchronized warm-serving baseline tuning.  
**Initial Approach**: Keep the earlier thesis that BoundFetch mainly needs more recall tuning and may already dominate the important warm-serving region.  
**Date**: 2026-04-14  
**Rounds**: 2 local refinement rounds  
**Final Verdict**: READY FOR NEXT EXECUTION ROUND  

## Problem Anchor
- Evaluate BoundFetch as a warm steady-state integrated `search + payload` system.
- Keep strong baselines in scope even when they are currently stronger.
- Determine whether the next improvement should be a minimal structural change in the hot path, or a broader repositioning of the paper.

## Output Files
- Review summary: `refine-logs/REVIEW_SUMMARY.md`
- Final proposal: `refine-logs/FINAL_PROPOSAL.md`
- Experiment plan: `refine-logs/EXPERIMENT_PLAN.md`
- Experiment tracker: `refine-logs/EXPERIMENT_TRACKER.md`

## Round-by-Round Review Record

| Round | Main Concern | What Was Changed | Result |
|-------|--------------|------------------|--------|
| 1 | The old proposal overfit to cold-start and broad disk-ANN claims. | Tightened the thesis to warm-only E2E serving, moved cold-start to non-goal, and shifted the next optimization target toward recall-latency tradeoff. | Resolved |
| 2 | The synchronized baseline results show tuned DiskANN+FlatStor dominating the current BoundFetch frontier, so the old "strong warm win" thesis is no longer accurate. | Re-anchored the proposal around a minimal warm-resident preload of `.clu` quantized vectors and address metadata, and replaced the full-matrix baseline idea with a layered comparison strategy. | Resolved for the next stage |

## Final Proposal Snapshot
- BoundFetch still clearly beats the IVF+PQ+FlatStor family on the warm E2E frontier, but tuned DiskANN+FlatStor is now the strong serving upper baseline.
- The next high-value BoundFetch optimization is not more blind submit-path tuning; it is warm-resident preload of cluster-side quantized vectors and raw address metadata.
- DiskANN should remain in the paper as a required strong reference rather than being removed because it currently wins.
- Storage-backend comparisons should be separated from search-core comparisons so the final story stays interpretable.

## Method Evolution Highlights
1. Shifted the paper target from cold-start disk behavior to warm steady-state serving.
2. Replaced the outdated "BoundFetch already wins the key warm comparison" thesis with a more honest "BoundFetch needs one minimal structural improvement to challenge the tuned DiskANN frontier" thesis.
3. Replaced a potentially explosive baseline matrix with a layered baseline plan that separates search quality, storage backend, and build-time cost.

## Pushback / Drift Log

| Round | Drift Candidate | Response | Outcome |
|-------|-----------------|----------|---------|
| 1 | Keep cold-start as a required protocol despite deployment mismatch. | Rejected; cold-start remains an explicit non-goal because it is operationally unsupported and misaligned with the intended deployment setting. | Rejected |
| 1 | Continue broad low-level optimization as the main next step. | Rejected; the early profile data already showed diminishing returns from more `io_uring` micro-tuning. | Rejected |
| 2 | Drop DiskANN from the baseline set because it is currently stronger and FlatStor is experimental. | Rejected; a stronger baseline should stay as the serving upper reference. The right response is layered evaluation, not baseline removal. | Rejected |
| 2 | Expand to a full `IVF+PQ/RQ/(DiskANN) x Lance/Parquet/(FlatStor)` comparison matrix. | Rejected; this is too expensive and muddies the story. Use one main search-core comparison plus one separate storage-backend block. | Rejected |

## Remaining Weaknesses
- Preloading `.clu` metadata may still be insufficient to recover a non-dominated region against DiskANN.
- If DiskANN remains strictly better on warm E2E and the build-time advantage is not compelling enough, the paper story may need to narrow further.
- Backend alignment with `Lance` or `Parquet` may cost non-trivial engineering time.

## Next Steps
- Implement warm-resident preload for `.clu` quantized vectors and raw address metadata.
- Re-run the BoundFetch Pareto frontier with preload enabled before doing any additional micro-optimization.
- Keep DiskANN in the reported baseline set.
- Run layered baseline experiments:
  - main search-core table
  - storage-backend ablation
  - build-time and preload-footprint table
