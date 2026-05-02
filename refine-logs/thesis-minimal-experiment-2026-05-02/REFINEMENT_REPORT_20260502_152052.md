# Refinement Report

**Problem**: 为本科毕业论文收敛 VectorRetrival/BoundFetch 的最小完整实验方案。  
**Date**: 2026-05-02  
**Rounds**: 2  
**Final Score**: 8.2 / 10  
**Final Verdict**: AUTHOR-REVISED after user supplement; ready for experiment planning, not externally re-reviewed

## Output Files

- Review summary: `refine-logs/thesis-minimal-experiment-2026-05-02/REVIEW_SUMMARY.md`
- Final proposal: `refine-logs/thesis-minimal-experiment-2026-05-02/FINAL_PROPOSAL.md`
- Score history: `refine-logs/thesis-minimal-experiment-2026-05-02/score-history.md`
- Raw reviews: `round-1-review.md`, `round-2-review.md`
- User-argue refinement: `round-3-refinement.md`
- User-supplement refinement: `round-4-refinement.md`

## Score Evolution

| Round | Problem Fidelity | Method Specificity | Contribution Quality | Frontier Leverage | Feasibility | Validation Focus | Venue/Thesis Readiness | Overall | Verdict |
|-------|------------------|--------------------|----------------------|-------------------|-------------|------------------|-------------------------|---------|---------|
| 1 | 8 | 6 | 6 | 7 | 6 | 5 | 6 | 6.4 | REVISE |
| 2 | 9 | 8 | 8 | 8 | 8 | 9 | 8 | 8.2 | READY |

Round 3 is an author-side refinement after user argue, not a new external review score.

Round 4 is an author-side refinement after user supplement, not a new external review score.

## Final Proposal Snapshot

- Method name: `BoundFetch-Guarded`
- Main claim: RaBitQ bound-guided SafeIn/SafeOut/Uncertain selective fetch plus dynamic io_uring scheduling lowers matched-quality E2E latency.
- Mandatory experiment: COCO100K + MS MARCO, each against `IVF+PQ/IVF+RaBitQ x FlatStor/Lance`.
- Required ablations: triage boundary/rerank pruning/payload prefetch ablation and dynamic scheduling ablation.
- Top-k policy: `topk=10` main, `topk=20` main-text supplement, `topk=50` appendix stress test.
- Strong target: `deep1m_synth` payload size sensitivity, with COCO synthetic padding fallback.

## Pushback / Drift Log

| Round | Reviewer Said | Author Response | Outcome |
|-------|---------------|-----------------|---------|
| 1 | Lance/MS MARCO and large sweep drift into benchmark study | Downgraded to appendix/backlog | Accepted |
| 1 | Method needs fixed protocol | Added top-C retrieve-fetch-rerank protocol | Accepted |
| 2 | Need matched-recall/cache/top-C execution rules | Wrote fixed rules into FINAL_PROPOSAL | Accepted |
| 3 | User argued COCO-only is too narrow | Restored COCO100K + MS MARCO mandatory main experiments | Accepted |
| 3 | User clarified current method is not fixed top-C | Replaced top-C protocol with bound-guided SafeIn/SafeOut/Uncertain triage | Accepted |
| 4 | User questioned outline placement | Moved workload differences to Chapter 6 and moved CRC/RAIR boundary to discussion | Accepted |
| 4 | User asked about top20/top50 | Added topk=20 as supplemental main-text point and topk=50 as appendix stress | Accepted |
| 4 | User asked whether ablations are too few | Expanded ablation hierarchy around triage boundaries, rerank pruning, prefetch, and scheduling | Accepted |

## Remaining Weaknesses

- `msmarco_passage` embedding/query/qrels or ANN ground truth readiness must be verified.
- Lance four-combination runner reuse must be verified.
- `deep1m_synth` path/name mismatch remains: user requested `deep1m_synth`, local directory currently shows `deep8m_synth`.
- Current implementation uses static calibrated `d_k_` for SafeIn and dynamic `dynamic_d_k` for SafeOut; the paper must state this accurately.
- Current `bench_e2e` recall output is fixed at `@1/@5/@10`; `recall@20/@50` needs code/output extension or prediction post-processing.

## Next Steps

1. Confirm MS MARCO assets and qrels/ground truth.
2. Run COCO100K full four-baseline matrix first.
3. Replicate the same matrix on MS MARCO.
4. Extend or post-process recall metrics for `@20/@50` before claiming them formally.
5. Run triage and dynamic scheduling ablations on COCO, then one key point on MS MARCO.
