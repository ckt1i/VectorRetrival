# Pipeline Summary

**Problem**: BoundFetch now already reaches the `0.97-0.99` recall band on the refreshed main curve, so the remaining method problem is query-side CPU cost rather than missing baseline evidence.  
**Final Method Thesis**: This workstream now focuses only on continued BoundFetch optimization, targeting query-prep and probe hotspots under fixed baseline evidence.  
**Final Verdict**: READY  
**Date**: 2026-04-16

## Ownership Split
- This workstream: BoundFetch method optimization
- Parallel workstream: all baseline experiments and figure/table maintenance

## Final Deliverables
- Proposal: `refine-logs/FINAL_PROPOSAL.md`
- Review summary: `refine-logs/REVIEW_SUMMARY.md`
- Experiment plan: `refine-logs/EXPERIMENT_PLAN.md`
- Experiment tracker: `refine-logs/EXPERIMENT_TRACKER.md`

## Contribution Snapshot
- Dominant contribution: continued CPU hot-path optimization on top of the existing warm-serving path
- Optional supporting contribution: none in this workstream
- Explicitly rejected complexity:
  - cold-start protocol
  - giant benchmark expansion before the core result is stable
  - more queue-depth / SQPOLL tuning as primary work

## Must-Prove Claims
- C1: the next meaningful gain must come from query-prep / probe optimization
- C2: `QuantizeQuery14Bit`, `PrepareQueryRotatedInto`, and probe itself are the current optimization targets

## First Runs to Launch
1. Do not continue `fastscan-lut-fusion-optimization`.
2. Re-open CPU optimization only with a different hypothesis.
3. If a new hypothesis is credible, re-measure the two high-value points on the current main curve.

## Main Risks
- Risk: recall does not improve without a meaningful latency jump.
  - Mitigation: freeze the code path and write the paper around the current operating region honestly.
- Risk: the comparison is read as unfair because the baseline and BoundFetch sit at different recall levels.
  - Mitigation: report the full Pareto instead of relying on one selected point.
- Risk: the story regresses into a tuning log.
  - Mitigation: keep all appendix-style tuning checks secondary to the main E2E claim.

## Next Action
- Stop the FastScan LUT fusion line and keep the rolled-back codebase as the active baseline
