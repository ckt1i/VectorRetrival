# Experiment Tracker

**Date**: 2026-04-10
**System**: BoundFetch

---

## Status Legend
- [ ] Not started
- [~] In progress
- [x] Complete
- [!] Blocked
- [-] Skipped (with reason)

---

## Phase 1: Development Validation (Target: Week 1-2)

### E1-dev: Main Result on SIFT1M
- [ ] Build index on SIFT1M with 1KB/10KB/100KB synthetic payloads
- [ ] Run BoundFetch: top_k={1,10,100}, nprobe={8,16,32,64}
- [ ] Run Eager-Fetch baseline (same index, classification disabled)
- [ ] Run Sequential-Fetch baseline (search first, then payload fetch)
- [ ] Collect: recall, QPS, latency, I/O bytes, classification stats
- [ ] **Gate check**: I/O reduction ≥20%?

### E3-dev: Classification Ablation on SIFT1M
- [ ] Run Full (3-class), Binary-Out, No-Class, Eager configs
- [ ] Collect classification distribution (SafeIn/Uncertain/SafeOut rates)
- [ ] Collect I/O bytes per query for each config
- [ ] **Gate check**: 3-class ≥10% better than Binary-Out?

---

## Phase 2: Core Ablations (Target: Week 3-4)

### E4: Layout Ablation
- [ ] Implement separated layout variant (separate .vec + .payload files)
- [ ] Implement vector-in-cluster layout variant
- [ ] Build all 3 layouts on SIFT10M
- [ ] Run with payload sizes {1KB, 10KB, 100KB}
- [ ] Collect latency, I/O ops, I/O bytes
- [ ] **Gate check**: Co-located ≥1.5x better at 10KB?

### E5: Multi-Stage Quantization Ablation
- [ ] Run 1-bit only, 1+2 bit, 1+4 bit, 4-bit only on SIFT10M
- [ ] Run same on GIST1M (high-dim test)
- [ ] Collect Uncertain rate, SafeIn rate, recall, QPS
- [ ] **Gate check**: ExRaBitQ reduces Uncertain by ≥15%?

### E6: CRC Early Stop Ablation
- [ ] Run CRC ON vs OFF on SIFT10M, nprobe={16,32,64,128}
- [ ] Run same on Deep10M
- [ ] Collect actual clusters probed, recall, QPS

### E2: Overlap Analysis
- [ ] Run on SIFT10M with 10KB payloads
- [ ] Vary prefetch_depth={1,2,4,8,16,32}
- [ ] Collect CPU utilization, I/O wait time, overlap ratio
- [ ] Profile with perf for time breakdown
- [ ] **Gate check**: Overlap ratio >60%?

---

## Phase 3: Baseline Comparison (Target: Week 5-6)

### E7: Baseline Setup
- [ ] Build DiskANN index on SIFT1M, Deep10M, SIFT100M
- [ ] Build SPANN index on same datasets
- [ ] Build FAISS IVF-PQ on-disk index on same datasets
- [ ] Verify all baselines achieve reported recall numbers

### E7: Baseline Runs
- [ ] Run all systems on SIFT1M (10KB payloads), collect metrics
- [ ] Run all systems on Deep10M (10KB payloads), collect metrics
- [ ] Run all systems on SIFT100M (10KB payloads), collect metrics
- [ ] Measure payload fetch latency separately for baselines
- [ ] Generate Pareto curves (recall vs QPS, with and without payload)

### E1-full: Main Result at Scale
- [ ] Run BoundFetch on Deep10M with full parameter sweep
- [ ] Run BoundFetch on SIFT100M with full parameter sweep
- [ ] Generate main result tables

---

## Phase 4: Scale & Polish (Target: Week 7-8)

### E8: Scalability Study
- [ ] Run SIFT1M → 10M → 100M at fixed recall target
- [ ] Collect QPS, latency, I/O bytes, build time, index size
- [ ] Generate scalability plots

### E9: Real-World Payloads (Optional)
- [ ] Prepare COCO dataset with CLIP embeddings + image payloads
- [ ] Build BoundFetch index
- [ ] Run multimodal retrieval benchmark
- [ ] Collect end-to-end latency with real payload distribution

---

## Paper Figures Checklist
- [ ] Figure 1: System architecture diagram
- [ ] Figure 2: Recall-QPS Pareto curves (E7)
- [ ] Figure 3: Latency breakdown stacked bar (E2)
- [ ] Figure 4: I/O volume with/without classification (E3)
- [ ] Figure 5: Classification distribution (E5)
- [ ] Figure 6: Scalability curves (E8)
- [ ] Table 1: Main results
- [ ] Table 2: I/O breakdown
- [ ] Table 3: Ablation summary
