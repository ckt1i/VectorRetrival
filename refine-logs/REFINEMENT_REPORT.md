# Refinement Report

**Date**: 2026-04-10 (v3: updated with preliminary experiments)

---

## Starting Point

User-provided innovation points:
1. ConANN cluster probing pruning + RaBitQ error bounds for multi-stage pruning and prefetch thresholds
2. Data layout: FlatBuffers, co-located vectors+payloads, clustered quantized codes+addresses
3. io_uring CPU-I/O decoupling for parallel vector search and data prefetch

## Refinement Trajectory

### Step 1: Identify the Unifying Insight

> **Quantization error bounds create a confidence spectrum that drives I/O scheduling decisions.**

- Point 1 (ConANN + RaBitQ bounds) → **decision mechanism**
- Point 2 (data layout) → **I/O targets**
- Point 3 (io_uring overlap) → **execution model**

### Step 2: Establish Contribution Hierarchy

- **Dominant**: Bound-driven three-class I/O scheduling (bounds → SafeOut/SafeIn/Uncertain → differentiated I/O)
- **Supporting**: Co-designed storage layout, single-thread io_uring overlap, multi-stage FastScan→ExRaBitQ

### Step 3: Sharpen Problem Statement

**Before**: "Disk ANNS optimization"
**After**: "Minimize wasted I/O in disk vector search with payload co-retrieval, using quantization error bounds as scheduling signals"

### Step 4: Name the System — **BoundFetch**

### Step 5: Position Against Related Work

| System | Index | I/O Model | Payload | Our Advantage |
|--------|-------|-----------|---------|---------------|
| DiskANN | Graph | Sector-aligned | Post-search | Bound-driven scheduling eliminates sequential payload trip |
| SPANN | IVF+postings | Batch reads | Separated | Classification reduces wasted I/O |
| Starling | Two-tier graph | Block reads | Not addressed | Payload co-retrieval as first-class |
| PipeANN | Graph | SSD-aligned | Not addressed | IVF batch scanning more schedulable |
| RaBitQ | (Quantization) | N/A | N/A | Extend bounds from distance to I/O scheduling |

### Step 6: Reject Complexity

- ~~Graph index~~ — pointer-chasing defeats batch I/O scheduling
- ~~Multi-threading~~ — focus on single-query latency; throughput via query-level parallelism
- ~~Learned routing~~ — bounds are provable, learned routing is not
- ~~GPU offload~~ — I/O is bottleneck, not compute
- ~~Filter predicates~~ — orthogonal, future work
- ~~Multi-node~~ — single-node scope sufficient

### Step 7: Preliminary Experiment Corrections (v2→v3)

**Key findings:**
1. **SafeOut is the dominant mechanism**: Stage 1 prunes 90-98% candidates
2. **SafeIn ≈ 0% in current configs**: Retained for theoretical completeness + exploratory C7
3. **CRC early-stop inversely related to top_k**: top_k=10→73%, top_k=20→21%
4. **Single-thread scope**: Documented as explicit design choice

**Narrative impact:**
- Original: "Three-class classification each plays a role"
- Revised: "SafeOut pruning is the core value; SafeIn is a mechanism with potential awaiting the right conditions"
- Added top_k sensitivity as a new analysis dimension

## Final Method Summary

BoundFetch uses RaBitQ error bounds for three-class I/O scheduling (SafeOut: skip, Uncertain: vector-only + on-demand payload, SafeIn: full record prefetch), executed via single-thread io_uring overlap on a co-designed dual-file layout. Preliminary experiments show SafeOut pruning (90-98%) is the primary I/O savings source, CRC dynamic threshold provides strong early-stop at small top_k (73%), and SafeIn activation conditions require further exploration.
