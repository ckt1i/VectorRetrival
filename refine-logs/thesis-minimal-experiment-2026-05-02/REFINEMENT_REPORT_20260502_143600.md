# Refinement Report

**Problem**: 为本科毕业论文收敛 VectorRetrival/BoundFetch 的最小完整实验方案。  
**Date**: 2026-05-02  
**Rounds**: 2  
**Final Score**: 8.2 / 10  
**Final Verdict**: READY for thesis execution

## Output Files

- Review summary: `refine-logs/thesis-minimal-experiment-2026-05-02/REVIEW_SUMMARY.md`
- Final proposal: `refine-logs/thesis-minimal-experiment-2026-05-02/FINAL_PROPOSAL.md`
- Score history: `refine-logs/thesis-minimal-experiment-2026-05-02/score-history.md`
- Raw reviews: `round-1-review.md`, `round-2-review.md`

## Score Evolution

| Round | Problem Fidelity | Method Specificity | Contribution Quality | Frontier Leverage | Feasibility | Validation Focus | Venue/Thesis Readiness | Overall | Verdict |
|-------|------------------|--------------------|----------------------|-------------------|-------------|------------------|-------------------------|---------|---------|
| 1 | 8 | 6 | 6 | 7 | 6 | 5 | 6 | 6.4 | REVISE |
| 2 | 9 | 8 | 8 | 8 | 8 | 9 | 8 | 8.2 | READY |

## Final Proposal Snapshot

- Method name: `BoundFetch-Single`
- Main claim: integrated `retrieve -> fetch -> rerank` query plan lowers matched-recall E2E latency.
- Mandatory experiment: COCO100K + FlatStor + BoundFetch-Single vs `IVF+PQ` and `IVF+RaBitQ`.
- Required ablation: overlap on/off with all other knobs fixed.
- Second experiment: `deep1m_synth` payload size sensitivity, with COCO synthetic padding fallback.

## Pushback / Drift Log

| Round | Reviewer Said | Author Response | Outcome |
|-------|---------------|-----------------|---------|
| 1 | Lance/MS MARCO and large sweep drift into benchmark study | Downgraded to appendix/backlog | Accepted |
| 1 | Method needs fixed protocol | Added top-C retrieve-fetch-rerank protocol | Accepted |
| 2 | Need matched-recall/cache/top-C execution rules | Wrote fixed rules into FINAL_PROPOSAL | Accepted |

## Remaining Weaknesses

- `deep1m_synth` path/name mismatch remains: user requested `deep1m_synth`, local directory currently shows `deep8m_synth`.
- Need confirm whether current `bench_e2e` exposes explicit `C` or equivalent candidate/rerank budget.
- Need implement or identify a clean serial-fetch/overlap-off ablation path.

## Next Steps

1. Confirm top-C / candidate budget parameter in current binary.
2. Run COCO100K mandatory matrix first.
3. Only after the main table and overlap ablation are stable, decide whether to add Lance or MS MARCO appendix.
