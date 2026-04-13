# Experiment Plan

**Problem**: BoundFetch should be validated as a warm steady-state integrated search+payload system.  
**Method Thesis**: The main value of BoundFetch is end-to-end warm-serving integration, and the next meaningful improvement target is recall at roughly the current latency.  
**Date**: 2026-04-13

## Claim Map

| Claim | Why It Matters | Minimum Convincing Evidence | Linked Blocks |
|-------|----------------|-----------------------------|---------------|
| C1 | BoundFetch is a strong warm-serving E2E system, not just a search core. | A recall-latency Pareto curve showing at least one practically useful operating region where BoundFetch beats strong separated baselines on E2E latency. | B1 |
| C2 | The current remaining bottleneck is submit/probe CPU, so the next major gain should come from recall tuning rather than more kernel-style micro-optimization. | Compact mechanism attribution plus a small recall-improvement ablation showing movement in recall without large latency regression. | B2, B3 |

## Paper Storyline
- Main paper must prove:
  - BoundFetch should be judged in warm steady-state E2E mode.
  - The integrated design gives a favorable latency-recall tradeoff against strong separated baselines.
  - The current bottleneck and next engineering target are understood.
- Appendix can support:
  - queue-depth sensitivity
  - shared vs isolated mode
  - SQPOLL fallback observation
- Experiments intentionally cut:
  - cold-start / drop-cache protocol
  - giant dataset sweep before the core story is stable
  - additional low-level tuning that does not affect recall-latency tradeoff

## Experiment Blocks

### Block 1: Warm E2E Pareto on COCO 100K
- Claim tested: C1
- Why this block exists: This is the main anchor result that aligns with the actual deployment target.
- Dataset / split / task: `coco_100k`, top-k=10, warm steady-state serving.
- Compared systems:
  - BoundFetch with `nprobe` sweep
  - DiskANN-disk + FlatStor-sim
  - FAISS-IVFPQ-disk + FlatStor-sim
- Metrics:
  - primary: `recall@10`, `e2e_ms`, `p99_ms`
  - secondary: `vec_search_ms`, `payload_ms`
- Setup details:
  - BoundFetch `nprobe={50,100,150,200,250,300,500}` if available
  - keep `io_qd=64`, `mode=shared` as default
  - use the existing warm protocol and 500-query setting unless there is a strong reason to change
- Success criterion:
  - produce a clear Pareto curve
  - BoundFetch dominates DiskANN in latency and improves on its own current operating-point characterization
- Failure interpretation:
  - if no useful Pareto region appears, the paper story weakens and may need to pivot toward a narrower systems-mechanism paper
- Table / figure target:
  - Main Figure: warm recall-latency Pareto
  - Main Table: selected operating points
- Priority: MUST-RUN

### Block 2: Mechanism Attribution at the Best Warm Operating Point
- Claim tested: C2
- Why this block exists: Reviewers need to see why BoundFetch is fast and why more queue/kernel tuning is not the primary next lever.
- Dataset / split / task: `coco_100k`, best current BoundFetch configuration and 1-2 nearby points.
- Compared systems:
  - BoundFetch best point
  - optional nearby `nprobe` point for contrast
- Metrics:
  - `uring_submit_ms`
  - `probe_ms`
  - `parse_cluster_ms`
  - `rerank_ms`
  - `io_wait_ms`
  - `submit_calls`
- Setup details:
  - reuse the internal stats already available from the benchmark
  - summarize only the components that materially contribute
- Success criterion:
  - show that device I/O wait is negligible
  - show that submit/probe CPU dominates the remaining cost
- Failure interpretation:
  - if attribution is unstable or noisy, reduce the claim and keep only qualitative mechanism discussion
- Table / figure target:
  - Main or Appendix stacked breakdown figure
- Priority: MUST-RUN

### Block 3: Recall-Improvement Minimal Ablation
- Claim tested: C2
- Why this block exists: This decides whether more code optimization is worth doing and where.
- Dataset / split / task: `coco_100k`, warm steady-state.
- Compared systems:
  - current best config
  - 2-3 nearby settings over `CRC_alpha` / pruning threshold / optional rerank or fetch policy
  - one simplified variant if needed, such as relaxed pruning
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - `uring_submit_ms`
  - `submit_calls`
- Setup details:
  - keep `io_qd=64`, `mode=shared`
  - change one mechanism family at a time
- Success criterion:
  - find at least one point with higher recall and acceptable latency regression
  - or conclude clearly that the current recall ceiling is structural, not due to missing micro-tuning
- Failure interpretation:
  - if recall does not move without large latency loss, stop optimizing and reposition the paper around the current operating region
- Table / figure target:
  - Appendix ablation table
- Priority: MUST-RUN

### Block 4: Generality Check on Deep1M / Deep8M
- Claim tested: Supporting generality only
- Why this block exists: Prevent the paper from looking COCO-only without exploding the run budget.
- Dataset / split / task:
  - `deep1m` or `deep8m`, whichever is quicker to iterate and already instrumented
- Compared systems:
  - BoundFetch only, or BoundFetch plus one baseline point if cheap
- Metrics:
  - latency trend
  - recall trend
  - attribution consistency
- Setup details:
  - no full sweep unless Block 1 and 3 already look strong
- Success criterion:
  - show the mechanism is not a one-dataset artifact
- Failure interpretation:
  - keep this in appendix or cut if it delays the main story
- Table / figure target:
  - Appendix support
- Priority: NICE-TO-HAVE

### Block 5: Legacy Tuning Checks to Freeze
- Claim tested: None directly; this block exists to stop over-optimizing the wrong axis.
- Why this block exists: Formalize that qd sweep / submission mode / SQPOLL are now validation checks, not main future work.
- Dataset / split / task: `coco_100k`
- Compared systems:
  - qd sweep results already collected
  - shared vs isolated
  - SQPOLL requested/effective fallback
- Metrics:
  - `e2e_ms`
  - `submit_calls`
  - effective SQPOLL flag
- Success criterion:
  - freeze these choices and move on
- Failure interpretation:
  - only revisit if a future environment materially changes effective SQPOLL behavior
- Table / figure target:
  - Appendix note
- Priority: NICE-TO-HAVE

## Run Order and Milestones

| Milestone | Goal | Runs | Decision Gate | Cost | Risk |
|-----------|------|------|---------------|------|------|
| M0 | Freeze protocol and paper scope | Update docs and lock warm-only protocol | All future runs use warm-only and the same reporting fields | 0.5 day | Low |
| M1 | Establish main Pareto curve | Block 1 | If no useful Pareto region appears, stop and reassess the paper story before more runs | 1-2 days | Medium |
| M2 | Explain the current bottleneck | Block 2 | If attribution is stable, keep mechanism story; otherwise reduce the mechanism claim | 0.5-1 day | Low |
| M3 | Decide whether more code optimization is still worth it | Block 3 | If recall can improve at similar latency, continue tuning; else freeze code and move to writeup | 1-2 days | Medium |
| M4 | Add light generality / appendix support | Block 4 and optional Block 5 | Only run if M1-M3 support the main paper | 1-2 days | Medium |

## Compute and Data Budget
- Total estimated GPU-hours: 0
- Data preparation needs:
  - none for COCO main path
  - optional re-use of Deep1M / Deep8M existing artifacts
- Human evaluation needs: none
- Biggest bottleneck: not compute, but choosing a convincing recall-latency operating point

## Risks and Mitigations
- Risk: BoundFetch remains faster but clearly lower-recall than the strongest baseline.
  - Mitigation: prioritize the recall-improvement ablation before any more systems tuning.
- Risk: The paper looks like a profile log instead of a systems result.
  - Mitigation: keep attribution compact and subordinate to the main Pareto result.
- Risk: The plan grows again into a giant benchmark suite.
  - Mitigation: cut anything that does not directly change a reviewer's belief about C1 or C2.

## Final Checklist
- [x] Main paper tables are covered
- [x] Novelty is isolated
- [x] Simplicity is defended
- [x] Frontier contribution is explicitly not claimed
- [x] Nice-to-have runs are separated from must-run runs
