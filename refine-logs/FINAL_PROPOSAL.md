# Final Proposal: Bound-Driven I/O Scheduling for Disk-Based Vector Search with Payload Co-Retrieval

**Date**: 2026-04-10 (v3: updated with preliminary experiment data)
**Status**: READY
**Target Venue**: SIGMOD / VLDB / OSDI

---

## 1. Problem Anchor

In production vector search systems (RAG, multimodal retrieval, recommendation), queries must return not only the nearest vectors but also associated payloads (documents, metadata, images). On disk-based systems, **I/O accounts for 70–90% of query latency** (PageANN 2024, PipeANN OSDI 2025).

Existing systems either fetch payloads sequentially after search (adding a full I/O round-trip) or eagerly co-fetch everything (wasting >90% of I/O bandwidth on non-Top-K candidates).

**The gap**: No existing system uses the *confidence* of approximate distance estimation to make fine-grained, per-candidate I/O scheduling decisions.

---

## 2. Method Thesis

We use RaBitQ's controllable quantization error bounds as confidence signals for three-class per-candidate I/O scheduling: SafeOut pruning eliminates 90-98% of candidates, SafeIn prefetches complete records for high-confidence Top-K candidates, and Uncertain candidates undergo exact verification with on-demand payload retrieval — all executed asynchronously via io_uring in parallel with cluster probing.

---

## 3. Dominant Contribution

**Bound-driven three-class I/O scheduling**: RaBitQ's error bound `epsilon = c * 2^{-bits/2} / sqrt(dim)` provides per-candidate margins that drive three I/O actions:

- **SafeOut** (`approx_dist > d_k + 2*margin`): Guaranteed NOT in Top-K → skip (zero I/O). **Preliminary: 90-98% pruning rate.**
- **Uncertain** (between thresholds): Cannot determine → read vector only, compute exact distance, conditionally fetch payload.
- **SafeIn** (`approx_dist < d_k - 2*margin`): Guaranteed in Top-K → prefetch complete record.

### SafeIn Status and Potential

SafeIn rate is ≈0% in current configurations due to strict conditions (conservative d_k calibration, wide 1-bit margins, small top_k). SafeIn may activate with:
- Larger top_k (50/100/200): wider d_k threshold
- Higher quantization precision (8-bit): smaller margin
- Aggressive d_k calibration (P90 vs P99)
- Large payloads: greater I/O savings per SafeIn hit

**Positioning**: SafeIn provides theoretical completeness in the three-class framework and is retained for systematic experimental exploration (C7).

---

## 4. Supporting Contributions

### 4.1 Co-Designed Dual-File Storage Layout
- **ClusterStore (.clu)**: Quantized codes + address column per IVF cluster
- **DataFile (.dat)**: Raw vectors co-located with payload columns, 4KB page-aligned

### 4.2 Single-Thread Overlap Scheduler
- io_uring-based single-thread, single-ring model
- Three concurrent pipelines: cluster prefetch, candidate I/O dispatch, rerank consumption
- **Design scope**: Single-query latency optimization. Throughput scales linearly via query-level parallelism (orthogonal).

### 4.3 Multi-Stage Quantization Pipeline
- Stage 1 (FastScan): AVX-512 VPSHUFB batch-32, 90-98% SafeOut
- Stage 2 (ExRaBitQ): 2/4-bit refinement for Uncertain candidates
- Dynamic CRC threshold: Progressive SafeOut tightening

---

## 5. Intentionally Rejected Complexity

- **Graph index (DiskANN/Vamana)**: Pointer-chasing I/O prevents batch scheduling and bound-based pruning. See §5.1.
- **Multi-threading**: Orthogonal engineering. Single-thread isolates I/O scheduling contribution.
- **Learned routing / GPU / Distributed / Filtered search**: As before.

### 5.1 Why Graph Indexes Don't Suit Payload Co-Prefetch

| Property | IVF (BoundFetch) | Graph (DiskANN) |
|----------|-----------------|-----------------|
| Traversal | Batch scan all candidates in cluster | Per-node pointer chasing |
| I/O predictability | High: know cluster blocks ahead | Low: next node depends on current |
| Batch I/O scheduling | Yes: classify entire cluster | No: one node at a time |
| Error bound utilization | RaBitQ global bounds for SafeOut/SafeIn | No analogous mechanism |
| Payload prefetch timing | Parallel with cluster probing | Sequential after search complete |

---

## 6. Key Claims (updated with preliminary data)

| # | Claim | Type | Preliminary |
|---|-------|------|-------------|
| C1 | SafeOut pruning eliminates 90%+ candidates, reducing I/O 40-60% vs eager-fetch | Main | ✓ 90-98% |
| C2 | Single-thread overlap scheduler achieves >80% CPU-I/O overlap | Main | Pending |
| C3 | Co-designed layout achieves 2-3x lower latency vs separated | Layout | Pending |
| C4 | Multi-stage quantization tightens Uncertain rate | Ablation | ✓ Stage 2 further prunes |
| C5 | CRC dynamic threshold enables 30-73% cluster early-stop | Ablation | ✓ top_k dependent |
| C6 | search+payload end-to-end superior to existing systems | Competitive | Pending |
| C7 | SafeIn activates under specific conditions (large top_k, high-bit, large payload) | Exploratory | Pending |

---

## 7. Preliminary Experiment Anchors

### COCO 100K (768-dim, top_k=10, bits=4, CRC on)
```
recall@10=0.8925, latency=1.738ms, SafeOut S1=90.54%, SafeIn=0%
```

### Deep1M top_k=10 (96→128 dim, bits=4, CRC on)
```
recall@10=0.9230, latency=0.814ms, SafeOut S1=93.79%, early_stop=73%, SafeIn=0%
```

### Deep1M top_k=20 (96→128 dim, bits=4, CRC on)
```
recall@10=0.9630, latency=1.547ms, SafeOut S1=97.91%, early_stop=21%, SafeIn=0.03%
```

### Key Finding: SafeOut-CRC Inverse Relationship
| | top_k=10 | top_k=20 |
|--|----------|----------|
| SafeOut S1 | 93.79% | 97.91% |
| CRC early stop | 100% (27/100) | 21% (85.64/100) |
| Latency | 0.814ms | 1.547ms |
