# Final Proposal: Warm-Resident BoundFetch and Layered Baseline Evaluation

**Date**: 2026-04-14  
**Status**: UPDATED AFTER SYNCHRONIZED BASELINE TUNING  
**Target Venue**: SIGMOD / VLDB / systems-data crossover

## Problem Anchor
- Bottom-line problem: In production vector retrieval, the query path should be evaluated as `search + payload delivery`, not vector search in isolation.
- Must-solve bottleneck: After synchronized warm-serving tuning, BoundFetch clearly outperforms the IVF+PQ+FlatStor baseline family but is still dominated by tuned DiskANN+FlatStor on COCO 100K. The next step is to determine whether a minimal warm-resident preload of `.clu` quantized vectors and raw address metadata can close enough of that serving gap to recover a useful system position.
- Non-goals:
  - cold-start latency or drop-cache protocols
  - removing DiskANN from comparison just because it is currently stronger
  - benchmarking every `search x storage` combination in a full Cartesian grid
  - preloading full payload bodies into memory and then calling the result a disk-serving system
- Constraints:
  - the server does not provide `sudo`
  - warm steady-state is the only required serving protocol
  - the codebase should remain single-node and modest in complexity
  - payload data should remain on the normal serving path; only lightweight cluster-side metadata may be promoted into memory
- Success condition: Either show that BoundFetch with warm-resident cluster metadata reaches a useful non-dominated region relative to DiskANN while remaining clearly stronger than IVF-family baselines, or show an explicit build-time / simplicity / serving tradeoff where DiskANN remains the search upper bound but not the full-system winner on all axes.

## Technical Gap
The latest synchronized baseline results change the story in an important way:

1. The old thesis that BoundFetch simply beats strong separated baselines on warm E2E latency is no longer accurate.
2. The current BoundFetch frontier is clearly above FAISS-IVFPQ+FlatStor, but tuned DiskANN+FlatStor now defines the strongest observed warm frontier.
3. The remaining gap is unlikely to be closed by more queue-depth or submit-path micro-tuning alone; the current evidence points to structural per-query work in the `.clu` path.
4. Search-core effects and storage-format effects are now entangled. If we compare `BoundFetch + FlatStor` against `DiskANN + FlatStor`, we still do not know how much comes from the search strategy versus the backend format.

So the next paper-quality move is not to widen the system. It is to make one small structural improvement to BoundFetch and to redesign the baseline logic so that strong comparisons remain fair and interpretable.

## Method Thesis
- One-sentence thesis: The smallest next intervention is a warm-resident cluster cache that preloads `.clu` quantized vectors and raw address blocks before query execution, so BoundFetch can remove repeated cluster-side read/parse work from the hot path without preloading payload bodies.
- Why this is the smallest adequate intervention: It directly targets the current structural disadvantage against DiskANN, which already benefits from having its compressed search-side representation resident in memory.
- Why the evaluation plan must change too: The baseline story should separate two axes, search-core quality and storage-backend quality, instead of exploding into a large matrix that is expensive to build and hard to interpret.

## Contribution Focus
- Dominant contribution: A warm-resident BoundFetch serving path that caches cluster-side quantized vectors and address metadata before queries, while keeping payload access on the normal integrated path.
- Optional supporting contribution: A layered baseline methodology that keeps DiskANN as a required strong reference, keeps IVF+PQ/RQ as lower search baselines, and isolates storage-format effects in a separate comparison block.
- Explicit non-contributions:
  - claiming pure ANN superiority over tuned graph indexes
  - claiming FlatStor is the only meaningful storage backend
  - claiming every search family must be paired with every storage backend in the main paper

## Proposed Method
### Complexity Budget
- Frozen / reused backbone:
  - Existing IVF-based BoundFetch retrieval framework
  - Existing `.clu` / payload storage pipeline
  - Existing warm benchmark harness and synchronized baseline scripts
- New trainable components: none
- New system changes:
  - one warm-resident cluster cache for `.clu` quantized vectors
  - one warm-resident cache for raw address blocks or decode-ready address metadata
- Tempting additions intentionally not used:
  - full payload preloading
  - new index families inside BoundFetch
  - large scheduler redesign
  - a full `search x storage` combinational benchmark

### System Overview
1. During index open or benchmark warmup, preload cluster-side quantized vectors from the `.clu` file.
2. In the same warmup phase, preload raw address blocks or decode-ready address metadata needed to map cluster hits to payload locations.
3. Keep payload bytes themselves on the original integrated retrieval path.
4. During query execution, reuse the resident cluster cache, run the same BoundFetch candidate filtering logic, and only issue payload-side I/O for the surviving path.
5. Measure the resulting system as one warm steady-state `search + payload` pipeline.

### Core Mechanism
- Input / output:
  - Input: query vector, IVF state, resident `.clu` cache, payload-backed data file
  - Output: Top-K results plus payload access in one E2E latency measurement
- Architecture or policy:
  - resident cluster-side metadata
  - integrated payload fetch for the surviving candidates only
  - no change to the warm-serving protocol
- Two implementation levels:
  - `preload_codes`: cache quantized vectors only
  - `preload_codes_and_addr`: cache quantized vectors plus raw address blocks or decoded address metadata
- Why this is the main novelty:
  - it is not a generic cache story
  - it specifically rebalances what must remain on the hot path in an integrated vector-plus-payload system

### Baseline Philosophy
- Keep DiskANN in the paper as the strong serving upper baseline even if it currently wins.
- Do not let FlatStor be the only backend used in the final comparison story.
- Use a layered baseline structure:
  - Main search-core comparison: BoundFetch vs IVF+PQ/RQ vs DiskANN under one chosen serving backend or one clearly documented backend policy
  - Storage-backend comparison: compare `FlatStor`, `Lance`, and optionally `Parquet` for one or two representative search cores
  - Build/index comparison: compare build time, index size, and resident preload size separately from query latency

### Integration into the Downstream Pipeline
- BoundFetch should still be positioned as a serving-path system, not a standalone ANN core.
- The warm-resident cache should be loaded at startup or benchmark warmup and then reused across queries.
- The paper should explicitly report the preload memory footprint and preload time so this optimization remains honest.

### Execution-Facing Plan
- Implement cluster-side preload first; do not start with a new round of low-level `io_uring` tuning.
- Measure the latency breakdown before and after preload to verify that the removed work is actually on the critical path.
- Only after the preload results are known should we decide whether further BoundFetch tuning is worthwhile.
- Rework the baseline suite into a staged plan:
  - keep DiskANN as a must-report reference
  - keep IVF+PQ or IVF+RQ as the weaker classical family
  - move backend-format comparisons into a separate supporting block instead of a full matrix

### Failure Modes and Diagnostics
- Failure mode: preloading `.clu` metadata changes little.
  - Detect via unchanged `e2e_ms`, unchanged `uring_submit_ms`, and unchanged cluster-side timing.
  - Mitigation: stop structural caching work and conclude that BoundFetch is currently limited by search quality rather than hot-path metadata access.
- Failure mode: preload improves latency but memory cost is too large.
  - Detect via resident set size and startup preload time.
  - Mitigation: downgrade to partial cache mode or cluster-on-demand caching, and report the memory tradeoff explicitly.
- Failure mode: reviewers interpret backend choices as cherry-picking.
  - Detect via inability to explain why DiskANN uses one backend and BoundFetch another.
  - Mitigation: either align on one backend where feasible, or keep DiskANN+FlatStor as an explicit upper-bound reference and separate storage-backend ablations into another table.
- Failure mode: the experiment suite explodes into too many combinations.
  - Detect via a growing `search x storage` matrix with no clear main table.
  - Mitigation: freeze a two-stage baseline policy and treat any extra pairings as appendix-only.

### Novelty and Elegance Argument
The strongest updated paper story is no longer "BoundFetch already beats all strong warm baselines." The stronger and more defensible version is:

"In warm integrated serving, the remaining gap to graph-based search comes from what cluster-side state stays on the hot path. By preloading only the compressed cluster representation and address metadata, BoundFetch can move closer to the graph frontier without abandoning its simpler build path or integrated payload design."

This is still a focused systems contribution. It keeps the method small, preserves the production-style warm-serving anchor, and stays honest about the existence of a stronger graph baseline.

## Claim-Driven Validation Sketch
### Claim 1: Warm-resident cluster preload is the highest-value next BoundFetch optimization.
- Minimal experiment: `preload=off`, `preload_codes`, and `preload_codes_and_addr` on the best current BoundFetch settings.
- Baselines / ablations:
  - current BoundFetch best point
  - two preload modes
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - `uring_submit_ms`
  - resident memory
  - preload time
- Expected evidence:
  - reduced E2E latency and cluster-side overhead without changing recall
  - a meaningful movement of the BoundFetch Pareto frontier toward DiskANN

### Claim 2: DiskANN should remain in the paper, but the baseline story must be layered.
- Minimal experiment: one main search-core comparison and one storage-backend ablation, instead of a full Cartesian product.
- Baselines / ablations:
  - BoundFetch + selected backend
  - IVF+PQ or IVF+RQ + same backend
  - DiskANN + same backend if feasible, otherwise DiskANN+FlatStor as an upper-bound reference
  - one representative storage-backend ablation over `FlatStor`, `Lance`, and optionally `Parquet`
- Metrics:
  - `recall@10`
  - `e2e_ms`
  - build time
  - index size
- Expected evidence:
  - strong main-table comparison remains credible because DiskANN is retained
  - storage-format contributions become separately interpretable

### Claim 3: Build-time and system simplicity are now part of the paper's fallback value.
- Minimal experiment: compare build time, peak memory during build, startup preload time, and resident search-side memory.
- Baselines / ablations:
  - BoundFetch
  - DiskANN
  - one IVF-family baseline
- Metrics:
  - build time
  - peak RSS
  - index bytes
  - preload bytes
- Expected evidence:
  - even if DiskANN remains the serving upper bound, BoundFetch may still occupy a better build-time / integration / storage-efficiency point

## Experiment Handoff Inputs
- Must-prove claims:
  - the next meaningful BoundFetch improvement is warm-resident `.clu` preload, not more blind submit-path tuning
  - DiskANN must stay as a strong reported reference
  - search-core and storage-format effects should be disentangled in the final comparison
- Must-run ablations:
  - preload off vs preload codes vs preload codes+address metadata
  - refreshed BoundFetch Pareto after preload
  - layered baseline comparison
  - build-time and preload-footprint table
- Critical datasets / metrics:
  - COCO 100K as the primary warm-serving benchmark
  - `recall@10`, `e2e_ms`, `p99_ms`, `uring_submit_ms`, resident RSS, build time, preload time
- Highest-risk assumptions:
  - preload may not be enough to recover a useful frontier region
  - a full backend-aligned DiskANN comparison may be expensive to implement
  - storage-format comparisons can explode unless explicitly staged

## Compute & Timeline Estimate
- Estimated GPU-hours: 0
- Data / annotation cost: none for the next stage
- Timeline:
  - 1-2 days to implement and validate cluster-side preload
  - 1-2 days to rerun the BoundFetch frontier with preload enabled
  - 2-4 days for layered baseline comparison and build-time reporting
