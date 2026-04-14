# Experiment Plan

**Problem**: After synchronized warm-serving tuning, BoundFetch is clearly stronger than the IVF+PQ+FlatStor family but is still dominated by tuned DiskANN+FlatStor on COCO 100K.  
**Method Thesis**: The smallest next optimization worth doing is to preload `.clu` quantized vectors and raw address metadata before query execution, then re-evaluate BoundFetch under a layered baseline strategy that keeps DiskANN as a strong reference and separates storage-format effects from search-core effects.  
**Date**: 2026-04-14

## Claim Map

| Claim | Why It Matters | Minimum Convincing Evidence | Linked Blocks |
|-------|----------------|-----------------------------|---------------|
| C1 | Warm-resident preload of `.clu` cluster-side state is the highest-value next BoundFetch optimization. | `preload=off`, `preload_codes`, and `preload_codes_and_addr` move the BoundFetch frontier in the right direction without changing recall semantics. | B1, B2 |
| C2 | DiskANN must remain in the evaluation, but the comparison should be layered rather than expanded into a full `search x storage` matrix. | One credible main search-core table, one storage-backend ablation, and one build-cost table together explain the tradeoff better than a large benchmark grid. | B3, B4, B5 |

## Paper Storyline
- Main paper must prove:
  - warm steady-state E2E serving remains the correct evaluation target
  - the next meaningful BoundFetch optimization is cluster-side preload, not more blind `io_uring` micro-tuning
  - BoundFetch remains clearly stronger than IVF-family baselines
  - DiskANN remains the strong upper baseline and should not be removed
- Appendix can support:
  - additional storage-backend variants
  - residual queue-depth or mode sensitivity checks
  - generality on Deep1M / Deep8M
- Experiments intentionally cut:
  - cold-start / drop-cache protocol
  - full `IVF+PQ/RQ/(DiskANN) x Lance/Parquet/(FlatStor)` Cartesian benchmarking
  - another round of submit-path tuning before preload results are known

## Baseline Comparison Policy

The new baseline policy is intentionally layered.

- Keep `DiskANN` in the reported comparisons even if it currently dominates the warm frontier.
- Do not treat `FlatStor` as the only storage backend in the final story.
- Do not attempt the full search-backend Cartesian product unless automation makes it nearly free.
- Use three comparison layers instead:
  - **Layer A: main search-core comparison**
    - BoundFetch
    - IVF+PQ or IVF+RQ
    - DiskANN
    - one chosen storage backend or one clearly documented backend policy
  - **Layer B: storage-backend ablation**
    - one or two representative search cores
    - `FlatStor`, `Lance`, and optionally `Parquet`
  - **Layer C: build / preload cost table**
    - build time
    - index size
    - resident preload size
    - startup preload time

Recommended interpretation:
- If aligned `Lance` integration is feasible for all compared systems, prefer `Lance` as the production-facing main backend.
- If aligned backend integration is too expensive, keep `DiskANN+FlatStor` as the strong warm-serving upper-bound reference and move backend-format comparisons into Layer B.
- `Parquet` should be added only if the engineering cost is small; otherwise it belongs in appendix or future work.

## Experiment Blocks

### Block 1: Warm-Resident Cluster Preload
- Claim tested: C1
- Why this block exists: This is the smallest structural change that directly targets the current gap to DiskANN.
- Dataset / split / task: `coco_100k`, top-k=10, warm steady-state serving, `queries=1000`
- Compared systems:
  - BoundFetch `preload=off`
  - BoundFetch `preload_codes`
  - BoundFetch `preload_codes_and_addr`
- Metrics:
  - primary: `recall@10`, `e2e_ms`, `p99_ms`
  - secondary: `uring_submit_ms`, `probe_ms`, resident RSS, preload time
- Setup details:
  - start from current best known operating point: `nlist=2048, nprobe=200, crc-alpha=0.05, epsilon=0.99`
  - payload bodies are not preloaded
  - record resident memory growth and startup preload duration
- Success criterion:
  - preload reduces E2E latency or cluster-side time enough to move the BoundFetch frontier toward DiskANN
  - recall remains unchanged for the same search settings
- Failure interpretation:
  - if preload has little effect, the remaining gap is probably search-quality or algorithmic rather than hot-path metadata access
- Table / figure target:
  - Main or Appendix ablation table
  - Breakdown figure showing what moved after preload
- Priority: MUST-RUN

### Block 2: Refreshed BoundFetch Pareto After Preload
- Claim tested: C1
- Why this block exists: One preload ablation is not enough; we need to see whether the whole BoundFetch frontier shifts.
- Dataset / split / task: `coco_100k`, top-k=10, warm steady-state serving, `queries=1000`
- Compared systems:
  - BoundFetch current frontier
  - BoundFetch with preload enabled
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
- Setup details:
  - rerun a compact grid over `crc-alpha={0.01,0.02,0.05,0.08,0.1}` at `nprobe={200,500}`
  - only add `nlist` or `epsilon` refinement if preload materially changes the frontier shape
- Success criterion:
  - at least one new non-dominated BoundFetch point appears
- Failure interpretation:
  - if the refreshed frontier is still fully dominated by DiskANN, stop low-level tuning and reposition the paper around build-time / integration tradeoffs
- Table / figure target:
  - Updated main Pareto figure
- Priority: MUST-RUN

### Block 3: Main Search-Core Comparison
- Claim tested: C2
- Why this block exists: The paper needs one clean main-table comparison that reviewers can trust.
- Dataset / split / task: `coco_100k`, top-k=10, warm steady-state serving, `queries=1000`
- Compared systems:
  - BoundFetch with the best preload setting
  - IVF+PQ or IVF+RQ baseline
  - DiskANN
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - build time
- Setup details:
  - prefer one shared backend for all three if feasible
  - if not feasible, use a clearly documented backend policy and keep DiskANN+FlatStor as an upper-bound reference
  - main goal is interpretability, not maximal benchmark coverage
- Success criterion:
  - the main table clearly shows where BoundFetch sits relative to a weaker classical baseline and the strong graph baseline
- Failure interpretation:
  - if the table is still ambiguous, reduce scope and keep only the most defensible pairings
- Table / figure target:
  - Main Table: search-core comparison
- Priority: MUST-RUN

### Block 4: Storage-Backend Ablation
- Claim tested: C2
- Why this block exists: Search-core and backend-format effects must be separated or the story will look confounded.
- Dataset / split / task: `coco_100k`, representative warm-serving operating points
- Compared systems:
  - one or two representative search cores across `FlatStor`, `Lance`, and optionally `Parquet`
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - startup time
  - file size
- Setup details:
  - do not run the full matrix by default
  - start with BoundFetch and one IVF-family baseline
  - add DiskANN only if backend integration is already available
- Success criterion:
  - storage-format contribution is visible without overwhelming the main story
- Failure interpretation:
  - if this block becomes too expensive, keep it in appendix or reduce it to `FlatStor` vs `Lance`
- Table / figure target:
  - Appendix storage-backend table
- Priority: MUST-RUN

### Block 5: Build and Preload Cost Table
- Claim tested: C2
- Why this block exists: If DiskANN stays stronger on serving latency, BoundFetch still needs a credible systems tradeoff axis.
- Dataset / split / task: `coco_100k`
- Compared systems:
  - BoundFetch
  - DiskANN
  - one IVF-family baseline
- Metrics:
  - build time
  - peak RSS during build
  - index bytes
  - preload bytes
  - preload time
- Setup details:
  - report these metrics separately from query latency
  - treat build and preload cost as first-class evidence, not as a footnote
- Success criterion:
  - produce a clear table showing whether BoundFetch occupies a better build-time / startup / simplicity point
- Failure interpretation:
  - if BoundFetch also loses on build and startup cost, the paper scope must narrow again
- Table / figure target:
  - Main or Appendix cost table
- Priority: MUST-RUN

### Block 6: Generality Check on Deep1M / Deep8M
- Claim tested: supporting evidence only
- Why this block exists: prevent the paper from looking COCO-only if the main story stabilizes
- Dataset / split / task:
  - `deep1m` or `deep8m`
- Compared systems:
  - BoundFetch best point
  - optional DiskANN reference point
- Metrics:
  - recall trend
  - latency trend
- Setup details:
  - run only after Blocks 1-5 are stable
- Success criterion:
  - show the direction is not a COCO-only artifact
- Failure interpretation:
  - keep as appendix or cut if it delays the main story
- Table / figure target:
  - Appendix support
- Priority: NICE-TO-HAVE

## Run Order and Milestones

| Milestone | Goal | Runs | Decision Gate | Cost | Risk |
|-----------|------|------|---------------|------|------|
| M0 | Re-anchor the docs after synchronized baseline tuning | Update proposal, report, plan, and tracker | All future runs use the new layered-baseline policy | 0.5 day | Low |
| M1 | Implement and validate preload | Block 1 | Do not run more micro-optimization before this result is known | 1-2 days | Medium |
| M2 | Refresh the BoundFetch frontier | Block 2 | If no new non-dominated point appears, stop low-level optimization | 1-2 days | Medium |
| M3 | Produce the main search-core comparison | Block 3 | DiskANN stays in scope unless integration is impossible | 1-3 days | Medium |
| M4 | Separate backend and build-time effects | Blocks 4 and 5 | If these blocks become too large, reduce them rather than expanding the main table | 2-4 days | Medium |
| M5 | Add light generality support | Block 6 | Run only if M1-M4 already support the paper | 1-2 days | Medium |

## Compute and Data Budget
- Total estimated GPU-hours: 0
- Data preparation needs:
  - no new COCO preparation
  - optional backend conversion work for `Lance` or `Parquet`
- Human evaluation needs: none
- Biggest bottleneck: engineering time, not compute
- Biggest coordination rule: keep the experiment suite layered; do not let the search-backend matrix explode

## Risks and Mitigations
- Risk: preload changes little and the BoundFetch frontier remains fully dominated by DiskANN.
  - Mitigation: stop low-level tuning quickly and elevate build-time / simplicity / integration tradeoffs into the explicit paper story.
- Risk: reviewers question fairness if DiskANN uses `FlatStor` while other systems use another backend.
  - Mitigation: either align the backend in Layer A or explicitly label DiskANN+FlatStor as an upper-bound reference and show backend ablations separately.
- Risk: the baseline space becomes too large to execute cleanly.
  - Mitigation: freeze the layered baseline policy and reject full Cartesian expansion.
- Risk: `Parquet` becomes a sink for engineering time without changing conclusions.
  - Mitigation: add `Parquet` only after `FlatStor` vs `Lance` is already stable.

## Final Checklist
- [x] Main paper tables are covered
- [x] DiskANN remains in scope as a strong baseline
- [x] Search-core and storage-backend effects are separated
- [x] Nice-to-have runs are separated from must-run runs
- [x] Cold-start remains explicitly out of scope
