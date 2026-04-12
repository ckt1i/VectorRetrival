# Experiment Plan: BoundFetch

**Date**: 2026-04-10
**Grounded in**: FINAL_PROPOSAL.md, REVIEW_SUMMARY.md

---

## 0. Claims to Validate

| ID | Claim | Experiment | Priority |
|----|-------|-----------|----------|
| C1 | Bound-driven 3-class I/O scheduling reduces I/O volume by 40-60% vs. eager-fetch | E1 (Main), E3 (Ablation) | Critical |
| C2 | Overlap scheduler achieves >80% CPU-I/O overlap ratio | E2 (I/O Analysis) | Critical |
| C3 | Co-designed storage layout achieves 2-3x lower latency than naive layouts | E4 (Layout Ablation) | Critical |
| C4 | Multi-stage quantization reduces Uncertain rate by 30-50% | E5 (Stage Ablation) | Important |
| C5 | Dynamic CRC threshold reduces clusters probed by 30-50% | E6 (CRC Ablation) | Important |
| C6 | Competitive recall-latency vs. DiskANN/SPANN + payload co-retrieval benefit | E7 (Baseline Comparison) | Critical |

---

## 1. Hardware & Environment

### Required Setup
- **Machine**: Single-node server with NVMe SSD (Samsung 990 Pro or similar, ≥2TB)
- **CPU**: x86-64 with AVX-512 support (Intel Sapphire Rapids / AMD Zen 4+)
- **Memory**: 64GB RAM (index metadata in memory, data on disk)
- **OS**: Linux 6.x with io_uring support (kernel ≥5.1, recommend ≥6.1)
- **Page cache**: Controlled via `echo 3 > /proc/sys/vm/drop_caches` between runs

### Software Dependencies
- Build: CMake, GCC 12+ / Clang 16+ (C++17, AVX-512)
- Baselines: DiskANN (official repo), SPANN (official repo), FAISS (IVF-PQ on-disk)
- Metrics: perf, iostat, blktrace for I/O profiling

---

## 2. Datasets

| Dataset | Vectors | Dim | Size (vectors) | Payload Config | Purpose |
|---------|---------|-----|----------------|----------------|---------|
| SIFT1M | 1M | 128 | ~512MB | Synthetic 1KB/10KB/100KB | Development, fast iteration |
| GIST1M | 1M | 960 | ~3.6GB | Synthetic 1KB/10KB/100KB | High-dim behavior |
| Deep1M | 1M | 96 | ~384MB | Synthetic 1KB/10KB | Development |
| SIFT10M | 10M | 128 | ~5GB | Synthetic 1KB/10KB/100KB | Medium scale |
| Deep10M | 10M | 96 | ~3.8GB | Synthetic 10KB | Medium scale |
| SIFT100M | 100M | 128 | ~50GB | Synthetic 10KB | Large scale |
| Deep1B | 1B | 96 | ~384GB | Synthetic 1KB | Billion-scale (stretch) |
| COCO-Multimodal | ~330K | 768 | ~1GB | Real image payloads (10-500KB) | Real-world payload distribution |

**Payload generation**: For synthetic payloads, generate random byte arrays of specified sizes. This controls payload size as an independent variable.

---

## 3. Experiment Descriptions

### E1: Main Result — End-to-End Performance (Claims C1, C6)

**Goal**: Demonstrate BoundFetch's recall-latency-IO tradeoff on standard benchmarks.

**Configuration**:
- Datasets: SIFT1M, GIST1M, Deep10M, SIFT100M
- Payload sizes: 0 (vector-only baseline), 1KB, 10KB, 100KB
- Parameters: top_k ∈ {1, 10, 100}, nprobe ∈ {8, 16, 32, 64, 128}
- Queries: 10,000 per configuration (standard query sets)
- Metrics: Recall@K, QPS, P50/P95/P99 latency (ms), total I/O bytes, I/O ops count

**Comparison Systems**:
- **BoundFetch** (full system)
- **BoundFetch-NoPayload** (vector search only, no payload fetch — upper bound)
- **Eager-Fetch** (fetch full record for every candidate that passes coarse filter)
- **Sequential-Fetch** (search first, then fetch payloads for Top-K results)

**Key Plots**:
- Recall vs. QPS (Pareto frontier) — one plot per dataset, payload size as parameter
- Recall vs. I/O volume (bytes read per query)
- Latency CDF at fixed recall (e.g., 95% recall@10)
- I/O breakdown pie chart: SafeIn reads, Uncertain reads, wasted reads, payload reads

**Decision Gate**: If BoundFetch does not reduce I/O volume by ≥30% vs. Eager-Fetch at iso-recall, revisit classification thresholds.

---

### E2: I/O Overlap Analysis (Claim C2)

**Goal**: Quantify CPU-I/O overlap effectiveness.

**Configuration**:
- Dataset: SIFT10M with 10KB payloads
- Fixed: top_k=10, recall target=95%
- Vary: nprobe ∈ {16, 32, 64}, prefetch_depth ∈ {1, 2, 4, 8, 16, 32}

**Metrics**:
- CPU utilization during search (via perf)
- I/O wait time (from SearchStats.io_wait_time_ms)
- Overlap ratio = 1 - (io_wait_time / total_time)
- I/O bandwidth utilization (actual vs. device peak)
- Latency breakdown: probe_time, rerank_time, io_wait_time, total_time

**Key Plots**:
- Overlap ratio vs. prefetch_depth (line plot)
- Stacked bar chart: time breakdown (CPU probe, CPU rerank, I/O wait, I/O hidden)
- I/O bandwidth utilization vs. prefetch_depth

**Decision Gate**: If overlap ratio < 60% even at depth=16, investigate I/O submission batching or ring sizing.

---

### E3: Classification Ablation (Claim C1) — **MUST-RUN**

**Goal**: Isolate the value of 3-class classification vs. simpler strategies.

**Configurations** (all on SIFT10M, 10KB payloads, top_k=10):
| Config | SafeIn | SafeOut | Uncertain | Description |
|--------|--------|---------|-----------|-------------|
| Full | Yes | Yes | Yes (vec-only) | BoundFetch complete |
| Binary-In | Yes | No | Merged with SafeIn | Fetch full record if not SafeOut |
| Binary-Out | No | Yes | Merged with SafeIn | Fetch full record if not SafeOut |
| No-Class | No | No | All vec-only | Always read vector, then conditionally payload |
| Eager | N/A | N/A | N/A | Fetch full record for all candidates |

**Metrics**: Recall@10, QPS, I/O bytes/query, wasted I/O bytes (reads for non-Top-K)

**Key Plot**: Grouped bar chart comparing I/O volume and latency across configs.

**Decision Gate**: If Full ≤ 10% better than Binary-Out, simplify to binary classification in the paper.

---

### E4: Storage Layout Ablation (Claim C3) — **MUST-RUN**

**Goal**: Validate the dual-file (ClusterStore + DataFile) co-located layout.

**Layout Variants**:
| Layout | Vector Storage | Payload Storage | Address |
|--------|---------------|-----------------|---------|
| Co-located (ours) | In .dat with payload | In .dat with vector | Address column in .clu |
| Separated | Separate .vec file | Separate .payload file | Two address columns |
| Vector-only .clu | Vectors in .clu blocks | Separate .payload file | Inline in .clu |

**Dataset**: SIFT10M, payload sizes {1KB, 10KB, 100KB}

**Metrics**: End-to-end latency, I/O ops/query, I/O bytes/query, random read amplification

**Key Plot**: Latency vs. payload size for each layout variant.

**Decision Gate**: If co-located is not ≥1.5x better than separated at 10KB payloads, reconsider layout claims.

---

### E5: Multi-Stage Quantization Ablation (Claim C4)

**Goal**: Show that Stage 2 (ExRaBitQ) meaningfully reduces Uncertain rate.

**Configurations**:
| Config | Stage 1 | Stage 2 | Description |
|--------|---------|---------|-------------|
| 1-bit only | FastScan 1-bit | None | Single-stage |
| 1+2 bit | FastScan 1-bit | ExRaBitQ 2-bit | Two-stage |
| 1+4 bit | FastScan 1-bit | ExRaBitQ 4-bit | Two-stage (higher precision) |
| 4-bit only | Direct 4-bit | None | Single-stage high-precision |

**Dataset**: SIFT10M, GIST1M (high-dim stress test)

**Metrics**: Uncertain rate (%), SafeIn rate (%), SafeOut rate (%), recall@10, QPS, Stage 2 time overhead

**Key Plots**:
- Stacked bar: classification distribution (SafeIn/Uncertain/SafeOut) per config
- Scatter: Recall vs. QPS for each config

**Decision Gate**: If ExRaBitQ 2-bit reduces Uncertain by <15%, the two-stage claim is weak — consider downgrading to "optional optimization."

---

### E6: CRC Early Stop Ablation (Claim C5)

**Goal**: Validate dynamic threshold's impact on cluster pruning.

**Configurations**:
- CRC ON (dynamic d_k from estimate heap) vs. CRC OFF (static d_k from calibration)
- Vary nprobe: {16, 32, 64, 128}

**Dataset**: SIFT10M, Deep10M

**Metrics**: Clusters actually probed (vs. nprobe requested), recall@10, QPS, early_stopped rate

**Key Plot**: Actual clusters probed vs. nprobe, with and without CRC.

---

### E7: Baseline Comparison (Claim C6) — **MUST-RUN**

**Goal**: Position BoundFetch against state-of-the-art disk ANNS.

**Baselines**:
1. **DiskANN** (official C++ implementation, Vamana graph, SSD-optimized)
2. **SPANN** (official implementation, IVF + posting lists)
3. **FAISS IVF-PQ** (on-disk mode with memory-mapped index)
4. **BoundFetch** (our system)

**Datasets**: SIFT1M, Deep10M, SIFT100M (with 10KB synthetic payloads)

**Protocol**:
- Each system: build index, warm up, run 10K queries
- Drop page cache between systems
- Report: Recall@{1,10,100} vs. QPS, latency distribution
- For payload: measure additional time for payload retrieval after search
  - DiskANN/SPANN/FAISS: sequential post-search fetch
  - BoundFetch: included in search (zero additional latency)

**Key Plots**:
- Recall@10 vs. QPS (Pareto curves, all systems on one plot)
- Recall@10 vs. QPS with payload included (BoundFetch advantage visible)
- Bar chart: search-only latency vs. search+payload latency per system

**Decision Gate**: If BoundFetch is >2x slower than DiskANN at iso-recall (search-only), investigate IVF vs. graph gap before claiming superiority.

---

### E8: Scalability Study

**Goal**: Show behavior at increasing scale.

**Datasets**: SIFT1M → SIFT10M → SIFT100M → (optionally Deep1B)
**Fixed**: top_k=10, target recall=95%, payload=10KB

**Metrics**: QPS, latency, I/O bytes/query, index build time, index size on disk

**Key Plot**: Log-scale dataset size vs. metrics.

---

### E9: Real-World Payload Distribution (Optional)

**Goal**: Demonstrate practical value with real multimodal data.

**Dataset**: COCO with CLIP embeddings (768-dim) + original image payloads (JPEG, 10-500KB variable)

**Scenario**: "Multimodal retrieval" — query with text embedding, retrieve nearest images with their payloads.

**Metrics**: End-to-end latency including payload delivery, I/O waste ratio

---

## 4. Run Order and Decision Gates

```
Phase 1: Development Validation (1-2 weeks)
├── E1 on SIFT1M (sanity check, fast turnaround)
├── E3 Classification Ablation on SIFT1M
└── GATE: Does 3-class classification show ≥20% I/O reduction?
    ├── YES → Continue
    └── NO → Debug classification thresholds, check Uncertain rate

Phase 2: Core Ablations (1-2 weeks)
├── E4 Layout Ablation on SIFT10M
├── E5 Multi-Stage Ablation on SIFT10M + GIST1M
├── E6 CRC Ablation on SIFT10M
├── E2 Overlap Analysis on SIFT10M
└── GATE: Are all components contributing meaningfully?
    ├── YES → Continue to baselines
    └── NO → Remove non-contributing components, simplify story

Phase 3: Baseline Comparison (1-2 weeks)
├── Setup DiskANN, SPANN, FAISS baselines
├── E7 Baseline Comparison on SIFT1M, Deep10M
├── E1 Full on Deep10M, SIFT100M
└── GATE: Competitive at iso-recall?
    ├── YES → Scale up
    └── NO → Analyze gap, tune parameters, consider scope adjustment

Phase 4: Scale & Polish (1-2 weeks)
├── E7 on SIFT100M
├── E8 Scalability Study
├── E9 Real-world payloads (if time permits)
└── Final: Generate all paper figures
```

---

## 5. Compute Budget Estimate

| Experiment | GPU/CPU Hours | Storage | Priority |
|------------|--------------|---------|----------|
| E1 (Main, all datasets) | ~40 CPU-hours | 500GB | Critical |
| E2 (Overlap analysis) | ~8 CPU-hours | 50GB | Critical |
| E3 (Classification ablation) | ~16 CPU-hours | 50GB | Critical |
| E4 (Layout ablation) | ~24 CPU-hours | 150GB (3 layouts) | Critical |
| E5 (Multi-stage ablation) | ~12 CPU-hours | 50GB | Important |
| E6 (CRC ablation) | ~8 CPU-hours | 50GB | Important |
| E7 (Baseline comparison) | ~48 CPU-hours | 500GB (all systems) | Critical |
| E8 (Scalability) | ~24 CPU-hours | 500GB | Important |
| E9 (Real-world) | ~8 CPU-hours | 50GB | Optional |
| **Total** | **~188 CPU-hours** | **~1.5TB peak** | |

No GPU required. All experiments are CPU + NVMe SSD bound.

---

## 6. Key Metrics to Report in Paper

### Table 1: Main Results
- Recall@{1,10,100} × QPS × Latency P50/P99 × I/O bytes/query
- Rows: BoundFetch, Eager-Fetch, Sequential-Fetch, DiskANN, SPANN, FAISS
- Columns: per dataset

### Table 2: I/O Breakdown
- Per-query average: SafeIn reads, Uncertain reads, SafeOut skipped, wasted reads, total I/O
- Show that BoundFetch's wasted I/O is significantly lower

### Table 3: Ablation Summary
- Full vs. No-Classification vs. No-ExRaBitQ vs. No-CRC vs. No-Overlap vs. Separated-Layout
- Relative QPS and I/O volume

### Figure 1: System Architecture (not an experiment)
### Figure 2: Recall-QPS Pareto curves (E7)
### Figure 3: Latency breakdown stacked bar (E2)
### Figure 4: I/O volume bar chart with/without classification (E3)
### Figure 5: Classification distribution across datasets (E5)
### Figure 6: Scalability (E8)
