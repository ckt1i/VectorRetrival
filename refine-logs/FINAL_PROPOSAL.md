# Final Proposal: Warm-Steady-State Integrated Vector Search and Payload Co-Retrieval

**Date**: 2026-04-13  
**Status**: READY  
**Target Venue**: SIGMOD / VLDB / systems-data crossover

## Problem Anchor
- Bottom-line problem: In production vector retrieval, the query path should be evaluated as `search + payload delivery`, not vector search in isolation.
- Must-solve bottleneck: Existing separated pipelines either pay a second-stage payload retrieval cost after search, or waste work on candidates that do not survive to the final Top-K. Under the current deployment setting, the remaining latency bottleneck is no longer block-device wait, but the CPU path around integrated probing, submission, and candidate verification.
- Non-goals: This project does not target cold-start latency, strict drop-cache protocols, or first-query cache-miss behavior. It also does not try to win pure vector-search latency against graph indexes without payload delivery.
- Constraints: The server does not provide `sudo`; warm steady-state is the only required protocol. The codebase should remain single-node, modest in complexity, and focused on one dominant mechanism rather than many parallel contributions.
- Success condition: Show that under warm steady-state serving, BoundFetch delivers a better or more favorable recall-latency end-to-end tradeoff than strong separated baselines, and explain the gain with a compact mechanism-level analysis.

## Technical Gap
The earlier proposal over-emphasized cold I/O and a broad three-class scheduling story. The current experimental evidence says something sharper:

1. The meaningful setting is warm steady-state serving.
2. `io_wait` is already almost fully hidden; the practical bottleneck is submit-side CPU plus fixed probe-side cost.
3. The strongest available competitive comparison is not "who is best at cold-start", but "who gives the best end-to-end search+payload result under production-like steady state".
4. SafeIn is structurally interesting but not currently the center of evidence; SafeOut plus integrated submission/path design is the stronger dominant story.

So the paper should not widen into a generic disk I/O paper. It should narrow into a systems paper about integrated search+payload retrieval under warm serving conditions.

## Method Thesis
- One-sentence thesis: BoundFetch wins in warm steady-state because it integrates candidate filtering, payload access, and asynchronous submission into one retrieval path, reducing end-to-end work relative to separated search-then-fetch baselines.
- Why this is the smallest adequate intervention: The winning mechanism is already in the codebase; the next meaningful step is not adding more subsystems, but tightening recall at roughly the current latency and validating the mechanism with cleaner evidence.
- Why this route is timely: Production vector systems increasingly care about full serving latency rather than search-only latency, especially once payload delivery and metadata access are included.

## Contribution Focus
- Dominant contribution: A warm-serving integrated retrieval path that combines vector probing and payload co-retrieval into one end-to-end system and outperforms strong separated baselines on E2E latency.
- Optional supporting contribution: Submit-path optimization and mechanism attribution that explain where the remaining latency goes and why additional queue-depth or SQPOLL tuning is not the main next lever.
- Explicit non-contributions:
  - cold-start superiority
  - graph-search supremacy in pure ANN
  - large-scale kernel or OS tuning claims
  - SafeIn as the main claimed source of gain

## Proposed Method
### Complexity Budget
- Frozen / reused backbone:
  - Existing IVF-based BoundFetch retrieval framework
  - Existing dual-file / integrated storage format
  - Existing warm benchmark harness and baseline pipeline
- New trainable components: none
- New system components to avoid:
  - new indexing families
  - new concurrency models
  - GPU / distributed / learned controllers

### System Overview
1. Probe IVF clusters and compute candidate scores.
2. Use the current filtering path to suppress obviously unneeded candidate work.
3. Submit payload/vector reads through the optimized `io_uring` path.
4. Parse, rerank, and complete Top-K delivery inside the same integrated pipeline.
5. Measure the system as one E2E serving path under warm steady-state.

### Core Mechanism
- Input / output:
  - Input: query vector, index state, payload-backed data file
  - Output: Top-K retrieval results plus payload access in one E2E latency measurement
- Architecture or policy:
  - integrated probing + async submission + on-demand verification
  - shared submission mode as the default warm-serving path
- Training signal / loss: not applicable; this is a systems mechanism, not a learned model
- Why this is the main novelty:
  - the contribution is not just "use io_uring"
  - the contribution is that the search path and payload path are no longer evaluated or executed as separate stages

### Optional Supporting Component
- Component: submit-path optimization and batching
- Role: reduce per-query submission overhead so integrated retrieval remains competitive at low-millisecond latency
- Why it does not create contribution sprawl: it is evidence and engineering support for the dominant integrated-path claim, not a second independent paper

### Modern Primitive Usage
- Frontier primitive usage: absent
- Reason: this work does not benefit from forcing an LLM/VLM/RL component; the strongest story is an intentionally simple systems contribution

### Integration into the Downstream Pipeline
- BoundFetch should be positioned as a serving-path system, not a standalone ANN core.
- The evaluation target is the application-visible latency of returning usable results, including payload access.
- The baseline comparison must therefore remain end-to-end and warm-only.

### Execution-Facing Plan
- Keep the current optimized submission path.
- Spend additional optimization budget only if it clearly increases recall at roughly fixed latency.
- Prefer parameter-level and threshold-level tuning over structural redesign.

### Failure Modes and Diagnostics
- Failure mode: BoundFetch remains faster than DiskANN+FlatStor but stays materially below the target recall range.
  - Detect via recall-latency Pareto curve.
  - Mitigation: tune `CRC_alpha`, pruning thresholds, and lightweight rerank/fetch policy.
- Failure mode: further submit-path work yields negligible gains.
  - Detect via unchanged `uring_submit_ms`, `submit_calls`, and E2E latency.
  - Mitigation: stop systems micro-optimization and switch effort to recall improvement and evidence tightening.
- Failure mode: reviewers see the gain as recall tradeoff rather than a true systems win.
  - Detect via lack of near-iso-recall comparison points.
  - Mitigation: run a compact recall-improvement ablation and report Pareto curves instead of a single point.

### Novelty and Elegance Argument
The strongest paper version is not "we built many components for disk ANN." It is:

"Once the serving target is correctly defined as warm steady-state `search + payload`, an integrated retrieval path can beat separated baselines end-to-end, and the remaining bottleneck is submit/probe CPU rather than device I/O."

That is a tighter and more defensible contribution than a broader cold-I/O or universal-ANN claim.

## Claim-Driven Validation Sketch
### Claim 1: BoundFetch provides a better warm E2E Pareto than strong separated baselines.
- Minimal experiment: COCO 100K warm-only end-to-end comparison using BoundFetch sweep versus DiskANN+FlatStor and FAISS-disk+FlatStor.
- Baselines / ablations:
  - BoundFetch `nprobe` sweep
  - strongest separated baseline points already available
- Metric:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
- Expected evidence:
  - BoundFetch dominates at least one practically relevant operating region
  - BoundFetch clearly beats DiskANN+FlatStor on latency
  - FAISS-IVFPQ-disk is documented as recall-limited on COCO

### Claim 2: The remaining engineering value lies in recall improvement, not more submission-path micro-tuning.
- Minimal experiment: compact ablation over `CRC_alpha` / pruning policy / optional rerank behavior at fixed warm protocol.
- Baselines / ablations:
  - current best config
  - 2-3 nearby threshold settings
  - optional pruning-off or relaxed-threshold variant
- Metric:
  - recall improvement at comparable latency
  - `uring_submit_ms`, `submit_calls`, `probe_ms`
- Expected evidence:
  - recall can move meaningfully while E2E stays close
  - queue-depth / SQPOLL / submission-mode sweeps remain secondary validation, not main future work

## Experiment Handoff Inputs
- Must-prove claims:
  - BoundFetch should be evaluated as a warm-serving integrated E2E system
  - the paper's next gains should come from better recall-latency tradeoff, not lower raw submit overhead
- Must-run ablations:
  - recall-latency Pareto sweep
  - mechanism attribution
  - minimal recall-improvement ablation
- Critical datasets / metrics:
  - COCO 100K as primary warm serving benchmark
  - Deep1M / Deep8M only if needed for supporting trend or ablation generality
  - `recall@10`, `e2e_ms`, `p99_ms`, `uring_submit_ms`, `probe_ms`, `submit_calls`
- Highest-risk assumptions:
  - current recall ceiling may still be too low for a convincing main-paper comparison
  - the story weakens if no better near-iso-recall operating point is found

## Compute & Timeline Estimate
- Estimated GPU-hours: 0
- Data / annotation cost: none for the next stage
- Timeline:
  - 1-2 days for warm-only documentation cleanup and Pareto-ready planning
  - 2-4 days for compact recall-improvement and ablation runs
  - 1-2 days for result consolidation and figure generation
