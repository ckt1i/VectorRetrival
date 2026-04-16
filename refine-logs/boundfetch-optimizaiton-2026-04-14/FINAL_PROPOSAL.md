# Final Proposal: Continued BoundFetch Method Optimization

**Date**: 2026-04-16  
**Status**: METHOD-OPTIMIZATION STAGE AFTER MAIN-CURVE REFRESH  
**Target Venue**: SIGMOD / VLDB / systems-data crossover

> Ownership note: baseline experiments and their maintenance have been split into a parallel task. This proposal defines only the current method-optimization workstream and treats baseline results as fixed input evidence.

## Problem Anchor
- Bottom-line problem: In production vector retrieval, the query path should be evaluated as `search + payload delivery`, not vector search in isolation.
- Must-solve bottleneck: After preload, corrected `bits=4` rebuilds, and the refreshed `epsilon=0.90, nprobe=512` curve, BoundFetch already sits in the `0.97-0.99` recall band. The remaining bottleneck is query-side CPU hot-path cost, especially query preparation and probe.
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
- Success condition: Move the current high-recall BoundFetch points leftward under the same search semantics, or show clean evidence that the remaining query-side CPU hotspots have been squeezed enough that further low-level work is not justified.

## Technical Gap
The latest synchronized baseline results change the story in an important way:

1. The old thesis that BoundFetch simply beats strong separated baselines on warm E2E latency is no longer accurate.
2. The current BoundFetch frontier is clearly above FAISS-IVFPQ+FlatStor, but tuned DiskANN+FlatStor now defines the strongest observed warm frontier.
3. The remaining gap is unlikely to be closed by more queue-depth or submit-path micro-tuning alone; the current evidence points to structural per-query work in the `.clu` path.
4. Search-core effects and storage-format effects are now entangled. If we compare `BoundFetch + FlatStor` against `DiskANN + FlatStor`, we still do not know how much comes from the search strategy versus the backend format.

So the next paper-quality move is not to widen the experiment matrix. It is to apply one narrow round of hotspot-driven method optimization to BoundFetch itself.

## Method Thesis
- One-sentence thesis: The smallest next intervention is to reduce per-query fixed CPU work and probe hot-path cost while keeping the current preload-enabled `bits=4` serving path unchanged.
- Why this is the smallest adequate intervention: preload has already removed cluster-side I/O and parse from the main bottleneck; the profiler now points to `QuantizeQuery14Bit`, `PrepareQueryRotatedInto`, and probe itself.
- Why baseline planning is no longer part of this proposal: baseline execution has been delegated to a parallel workstream; this proposal only covers method-side improvement.

## Contribution Focus
- Dominant contribution: a finite, profiler-guided CPU hot-path optimization route for BoundFetch on top of the current serving path.
- Optional supporting contribution: none in this workstream.
- Explicit non-contributions:
  - claiming pure ANN superiority over tuned graph indexes
  - claiming FlatStor is the only meaningful storage backend
  - claiming every search family must be paired with every storage backend in the main paper

## Proposed Method
### Complexity Budget
- Frozen / reused backbone:
  - Existing IVF-based BoundFetch retrieval framework
  - Existing `.clu` / payload storage pipeline
  - Existing warm benchmark harness and completed baseline results
- New trainable components: none
- New system changes:
  - tighter per-query preparation
  - tighter query buffer / probe layout
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

### Baseline Input Contract
- DiskANN, FAISS, and IVF+RQ/ConANN results remain fixed comparison inputs.
- Any further baseline reruns, sweeps, or figure maintenance are outside this workstream.
- This proposal only requires re-measuring the optimized BoundFetch points against the already established reporting format.

### Integration into the Downstream Pipeline
- BoundFetch should still be positioned as a serving-path system, not a standalone ANN core.
- The warm-resident cache should be loaded at startup or benchmark warmup and then reused across queries.
- The paper should explicitly report the preload memory footprint and preload time so this optimization remains honest.

### Execution-Facing Plan
- Run R068 first: tighten `QuantizeQuery14Bit` and `PrepareQueryRotatedInto`.
- Only if R068 yields a clear gain, run R069: improve query/probe data layout and SIMD friendliness.
- Run R070 only if rerank or candidate materialization becomes newly visible after the first two passes.
- Do not start new baseline sweeps inside this workstream.

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
