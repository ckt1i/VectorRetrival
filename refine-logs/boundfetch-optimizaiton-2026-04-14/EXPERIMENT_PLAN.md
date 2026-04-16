# Experiment Plan

**Problem**: After synchronized warm-serving tuning, `.clu` full preload, corrected `bits=4` rebuilds, and the refreshed `epsilon=0.90, nprobe=512` main curve, BoundFetch already reaches the `0.97-0.99` recall band on COCO 100K. The remaining method task is to further reduce query-prep / probe CPU cost without changing search semantics.  
**Method Thesis**: The current workstream is now strictly method-side. Baseline experiments are handled by a parallel task. This plan only covers finite, hotspot-driven BoundFetch optimization plus the minimal re-evaluation needed after each optimization pass.  
**Date**: 2026-04-16

## Ownership Split

- **This workstream owns**: further BoundFetch optimization, especially query-prep / probe CPU hotspots.
- **Parallel workstream owns**: baseline experiments, baseline plots/tables, and any additional baseline sweeps.
- **Baseline-related sections below** remain as fixed inputs and coordination points, not as this workstream's execution checklist.

## Claim Map

| Claim | Why It Matters | Minimum Convincing Evidence | Linked Blocks |
|-------|----------------|-----------------------------|---------------|
| C1 | Warm-resident preload of `.clu` cluster-side state is a valid BoundFetch optimization, but no longer the main missing lever. | Preload preserves recall semantics and improves latency, yet the high-recall gap to DiskANN remains. | B1, B2 |
| C2 | The next important BoundFetch optimization target is the query-prep / probe CPU hot path rather than more cluster-side submit-path tuning. | R067 and the corrected `bits=4` sweep show that `.clu` I/O and rerank are no longer the main bottlenecks; the dominant query-side samples are `QuantizeQuery14Bit`, `PrepareQueryRotatedInto`, and probe itself. | B3 |
| C3 | Baseline results should now act as fixed input evidence rather than consume further execution budget in this workstream. | The current refreshed curve is already enough to position BoundFetch; further baseline expansion is delegated to the parallel track. | B4, B5, B6 |

## Paper Storyline
- Main paper must prove:
  - warm steady-state E2E serving remains the correct evaluation target
  - `.clu` preload is a useful completed optimization, not the final answer
  - the next meaningful BoundFetch optimization is query-prep / probe efficiency, not more blind `io_uring` micro-tuning
  - the next meaningful gain must come from method-side optimization rather than more comparison work
  - baseline evidence remains important, but further baseline execution is not part of this workstream
- Appendix can support:
  - additional storage-backend variants
  - residual queue-depth or mode sensitivity checks
  - graph-baseline reference numbers
  - generality on Deep1M / Deep8M and one real-data workload
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
    - IVF+PQ / IVF+RQ / ConANN-style IVF-family peers
    - optional DiskANN summary row if backend policy is aligned enough
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
- The main claim should be won first inside the IVF-family regime, because that is the regime most aligned with BoundFetch's fast-build and future dynamic-update positioning.
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

### Block 3: CPU-Side BoundFetch Optimization
- Claim tested: C2
- Why this block exists: After preload, the remaining gap is most plausibly in query-prep / probe CPU cost, especially around the new `recall@10 = 0.98+` region.
- Dataset / split / task: `coco_100k`, top-k=10, warm steady-state serving, `queries=1000`
- Compared systems:
  - BoundFetch current best preload setting
  - BoundFetch with one CPU-path optimization at a time
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - `probe_ms`
  - `rerank_cpu_ms`
  - candidates scanned / verified
- Setup details:
  - keep `clu_mode=full_preload`
  - anchor on the two high-value points from the current main curve:
    - `nlist=2048, nprobe=512, bits=4, epsilon=0.90, alpha=0.02`
    - `nlist=2048, nprobe=512, bits=4, epsilon=0.90, alpha=0.01`
  - implement only changes that can plausibly reduce query-prep or per-cluster probe CPU work without changing search semantics
  - likely candidates:
    - improve SIMD-friendly data layout for query buffers and probe
    - reduce redundant decode / bound-check work in probe
    - consider candidate materialization and rerank only if they become visible after the first two passes
  - already rejected direction:
    - `fastscan-lut-fusion-optimization`: both implementation passes missed the benchmark gate, and the second fused helper was slower; the code path has been rolled back
- Success criterion:
  - at least one `recall@10 >= 0.98` point moves materially toward DiskANN without harming recall semantics
- Failure interpretation:
  - if CPU-side tuning cannot move the `>=0.98` region enough, stop trying to beat DiskANN directly on warm latency
- Table / figure target:
  - Mechanism table + refreshed high-recall Pareto
- Priority: MUST-RUN

### Block 4: Synchronized IVF-Family Baselines (Parallel Track)
- Claim tested: C3
- Why this block exists: Before spending more time chasing DiskANN, we need to prove where BoundFetch sits among the methods it is most naturally compared against.
- Dataset / split / task: `coco_100k`, representative warm-serving operating points
- Compared systems:
  - BoundFetch best preload points
  - FAISS IVF+PQ
  - one additional IVF-family baseline if available, preferably IVF+RQ or ConANN
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - build time
- Setup details:
  - tune each baseline into its own Pareto curve under the same warm protocol
  - use the same `queries=1000`, `topk=10`, and matched `nlist` when applicable
  - produce one synchronized figure that contains:
    - BoundFetch
    - IVF+PQ
    - IVF+RQ / ConANN
- Success criterion:
  - establish that BoundFetch is the strongest point on the IVF-family tradeoff surface
- Failure interpretation:
  - if another IVF-family method dominates BoundFetch, prioritize understanding and fixing that gap before touching more datasets
- Table / figure target:
  - Main Pareto figure for IVF-family comparison
- Priority: PARALLEL TRACK

### Block 5: DiskANN Reference and Cost Table (Parallel Track)
- Claim tested: C3
- Why this block exists: DiskANN should stay visible, but as a strong graph reference alongside build/startup tradeoffs rather than the only main comparator.
- Dataset / split / task: `coco_100k`
- Compared systems:
  - BoundFetch
  - DiskANN
  - one tuned IVF-family baseline
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - build time
  - peak RSS during build
  - index bytes
  - preload bytes
  - preload time
- Setup details:
  - report these metrics separately from query latency
  - treat build and preload cost as first-class evidence, not as a footnote
- Success criterion:
  - produce a clear table showing whether BoundFetch occupies a better build-time / startup / simplicity point even if DiskANN stays faster in serving
- Failure interpretation:
  - if BoundFetch also loses on build and startup cost, the paper scope must narrow again
- Table / figure target:
  - Main cost table + secondary DiskANN reference figure
- Priority: PARALLEL TRACK

### Block 6: Storage-Backend Ablation (Parallel Track)
- Claim tested: C3
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
- Priority: PARALLEL TRACK

### Block 7: Generality Check Beyond COCO 100K
- Claim tested: supporting evidence only
- Why this block exists: the paper should not remain COCO-only once the main comparison story is stable.
- Dataset / split / task:
  - stage 1: `deep1m` or `deep8m`
  - stage 2: one real-data workload, preferably `MS MARCO Passage` or `Amazon Products`
- Compared systems:
  - BoundFetch best point
  - strongest tuned IVF-family baseline
  - optional DiskANN reference point
- Metrics:
  - recall trend
  - latency trend
  - build / preload trend
- Setup details:
  - run only after Blocks 1-5 are stable
  - start with synthetic scale-up before moving to real-data heterogeneity
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
| M0 | Re-anchor the docs after preload and retests | Update report, plan, and tracker | All future runs use the new post-preload policy | 0.5 day | Low |
| M1 | Lock in preload as completed optimization | Blocks 1 and 2 | Do not restart submit-path tuning unless new evidence appears | 0.5-1 day | Low |
| M2 | Decide whether CPU tuning is still worthwhile | Block 3 | `fastscan-lut-fusion-optimization` is already closed as failed; only continue with a new CPU-side hypothesis | 1-3 days | Medium |
| M3 | Consume parallel baseline outputs and keep the story aligned | Blocks 4 and 5 | Baseline outputs arrive externally; this workstream only updates the interpretation | 0.5-1 day | Low |
| M4 | Add light generality support | Block 7 | Run only if M1-M3 already support the paper | 2-4 days | Medium |

## Compute and Data Budget
- Total estimated GPU-hours: 0
- Data preparation needs:
  - no new COCO preparation
  - optional backend conversion work for `Lance` or `Parquet`
- Human evaluation needs: none
- Biggest bottleneck: engineering time, not compute
- Biggest coordination rule: keep the experiment suite layered; do not let the search-backend matrix explode
- Immediate bottleneck after preload: CPU-side search cost in the high-recall region

## Risks and Mitigations
- Risk: CPU-side tuning also fails to move the high-recall region enough.
  - Mitigation: stop trying to beat DiskANN directly on warm latency and reposition the main claim around IVF-family leadership plus build/startup advantages.
- Risk: reviewers question fairness if DiskANN uses `FlatStor` while other systems use another backend.
  - Mitigation: either align the backend in Layer A or explicitly label DiskANN+FlatStor as an upper-bound reference and show backend ablations separately.
- Risk: the baseline space becomes too large to execute cleanly.
  - Mitigation: freeze the layered baseline policy and reject full Cartesian expansion.
- Risk: `Parquet` becomes a sink for engineering time without changing conclusions.
  - Mitigation: add `Parquet` only after `FlatStor` vs `Lance` is already stable.
- Risk: real-data datasets introduce schema and payload variability that delays the main story.
  - Mitigation: defer real-data evaluation until the COCO + Deep synthetic pipeline is stable.

## Final Checklist
- [x] Main paper tables are covered
- [x] DiskANN remains in scope as a strong baseline
- [x] Search-core and storage-backend effects are separated
- [x] Nice-to-have runs are separated from must-run runs
- [x] Cold-start remains explicitly out of scope
