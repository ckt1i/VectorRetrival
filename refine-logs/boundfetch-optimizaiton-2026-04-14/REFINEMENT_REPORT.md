# Refinement Report

**Problem**: Re-anchor the refine documents after the work split so they describe only the method-optimization track.  
**Initial Approach**: Keep baseline expansion and method optimization in one mixed execution narrative.  
**Date**: 2026-04-16  
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
- These refine documents now serve the continued BoundFetch method-optimization track only.
- Baseline results remain fixed inputs and no longer define the active execution checklist here.
- The next high-value BoundFetch optimization is query-prep / probe CPU reduction, not more `.clu` / submit-path work.
- The active execution decision is now R068 first, then conditional R069 / R070.

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
- Execute R068 on the current main curve.
- Re-measure the two high-value BoundFetch points after R068.
- Only if R068 clearly helps, proceed to R069 and then optional R070.
