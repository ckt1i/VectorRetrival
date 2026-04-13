# Pipeline Summary

**Problem**: BoundFetch has completed the baseline stage and now needs a focused, warm-only experiment roadmap that matches the actual deployment setting.  
**Final Method Thesis**: BoundFetch should be treated as an integrated warm-serving `search + payload` system, and the next meaningful step is to improve or clarify the recall-latency Pareto rather than continue broad low-level optimization.  
**Final Verdict**: READY  
**Date**: 2026-04-13

## Final Deliverables
- Proposal: `refine-logs/FINAL_PROPOSAL.md`
- Review summary: `refine-logs/REVIEW_SUMMARY.md`
- Experiment plan: `refine-logs/EXPERIMENT_PLAN.md`
- Experiment tracker: `refine-logs/EXPERIMENT_TRACKER.md`

## Contribution Snapshot
- Dominant contribution: integrated warm steady-state end-to-end retrieval with payload co-delivery
- Optional supporting contribution: submit-path optimization as mechanism support, not as the main paper story
- Explicitly rejected complexity:
  - cold-start protocol
  - giant benchmark expansion before the core result is stable
  - more queue-depth / SQPOLL tuning as primary work

## Must-Prove Claims
- C1: BoundFetch has a favorable warm recall-latency E2E tradeoff versus strong separated baselines.
- C2: The remaining meaningful engineering question is recall improvement, not further low-level submission-path micro-optimization.

## First Runs to Launch
1. Add 1-2 intermediate BoundFetch operating points on COCO 100K to make the warm Pareto curve denser.
2. Run a compact recall-improvement ablation over `CRC_alpha` / pruning threshold while keeping `io_qd=64` and `mode=shared`.
3. Summarize one stable mechanism-attribution figure or table at the best operating point and one nearby contrast point.

## Main Risks
- Risk: recall does not improve without a meaningful latency jump.
  - Mitigation: freeze the code path and write the paper around the current operating region honestly.
- Risk: the comparison is read as unfair because the baseline and BoundFetch sit at different recall levels.
  - Mitigation: report the full Pareto instead of relying on one selected point.
- Risk: the story regresses into a tuning log.
  - Mitigation: keep all appendix-style tuning checks secondary to the main E2E claim.

## Next Action
- Proceed to `/run-experiment`
