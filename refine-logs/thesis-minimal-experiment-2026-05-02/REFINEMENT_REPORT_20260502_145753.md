# Refinement Report

**Problem**: 为本科毕业论文收敛 VectorRetrival/BoundFetch 的最小完整实验方案。  
**Date**: 2026-05-02  
**Rounds**: 2  
**Final Score**: 8.2 / 10  
**Final Verdict**: AUTHOR-REVISED after user argue; ready for experiment planning, not externally re-reviewed

## Output Files

- Review summary: `refine-logs/thesis-minimal-experiment-2026-05-02/REVIEW_SUMMARY.md`
- Final proposal: `refine-logs/thesis-minimal-experiment-2026-05-02/FINAL_PROPOSAL.md`
- Score history: `refine-logs/thesis-minimal-experiment-2026-05-02/score-history.md`
- Raw reviews: `round-1-review.md`, `round-2-review.md`
- User-argue refinement: `round-3-refinement.md`

## Score Evolution

| Round | Problem Fidelity | Method Specificity | Contribution Quality | Frontier Leverage | Feasibility | Validation Focus | Venue/Thesis Readiness | Overall | Verdict |
|-------|------------------|--------------------|----------------------|-------------------|-------------|------------------|-------------------------|---------|---------|
| 1 | 8 | 6 | 6 | 7 | 6 | 5 | 6 | 6.4 | REVISE |
| 2 | 9 | 8 | 8 | 8 | 8 | 9 | 8 | 8.2 | READY |

Round 3 is an author-side refinement after user argue, not a new external review score.

## Final Proposal Snapshot

- Method name: `BoundFetch-Guarded`
- Main claim: RaBitQ bound-guided SafeIn/SafeOut/Uncertain selective fetch plus dynamic io_uring scheduling lowers matched-quality E2E latency.
- Mandatory experiment: COCO100K + MS MARCO, each against `IVF+PQ/IVF+RaBitQ x FlatStor/Lance`.
- Required ablations: triage ablation and dynamic scheduling ablation.
- Strong target: `deep1m_synth` payload size sensitivity, with COCO synthetic padding fallback.

## Pushback / Drift Log

| Round | Reviewer Said | Author Response | Outcome |
|-------|---------------|-----------------|---------|
| 1 | Lance/MS MARCO and large sweep drift into benchmark study | Downgraded to appendix/backlog | Accepted |
| 1 | Method needs fixed protocol | Added top-C retrieve-fetch-rerank protocol | Accepted |
| 2 | Need matched-recall/cache/top-C execution rules | Wrote fixed rules into FINAL_PROPOSAL | Accepted |
| 3 | User argued COCO-only is too narrow | Restored COCO100K + MS MARCO mandatory main experiments | Accepted |
| 3 | User clarified current method is not fixed top-C | Replaced top-C protocol with bound-guided SafeIn/SafeOut/Uncertain triage | Accepted |

## Remaining Weaknesses

- `msmarco_passage` embedding/query/qrels or ANN ground truth readiness must be verified.
- Lance four-combination runner reuse must be verified.
- `deep1m_synth` path/name mismatch remains: user requested `deep1m_synth`, local directory currently shows `deep8m_synth`.
- Current implementation uses static calibrated `d_k_` for SafeIn and dynamic `dynamic_d_k` for SafeOut; the paper must state this accurately.

## Next Steps

1. Confirm MS MARCO assets and qrels/ground truth.
2. Run COCO100K full four-baseline matrix first.
3. Replicate the same matrix on MS MARCO.
4. Run triage and dynamic scheduling ablations on COCO, then one key point on MS MARCO.
