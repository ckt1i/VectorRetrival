## Context

The v4 baseline change (`bench-e2e-rerank-timing-and-baseline`) successfully delivered pure-memory FAISS IVF-PQ/IVF-Flat and payload retrieval (FlatStor/Lance/Parquet) benchmarks. However, the paper claim C6 "BoundFetch outperforms existing systems on search+payload E2E" requires a fair comparison against **disk-based** vector search systems. Pure-memory FAISS numbers cannot support this claim — a reviewer will immediately point out that BoundFetch is being compared to a fundamentally different operating mode.

Three concrete blockers for v4 have been resolved:
1. **Python 3.8 ceiling on diskannpy**: A new `labnew` conda env with Python 3.11 is now available, unblocking `diskannpy>=0.7` which has the modern `build_disk_index()` + `StaticDiskIndex` API and fixed memory cleanup bugs. (Python 3.11 is required because diskannpy>=0.7 depends on numpy==1.25, which has compatibility issues with Python 3.12.)
2. **Dataset too small to show I/O**: v4's Deep1M (384 MB) and COCO 100K (200 MB) both fit entirely in page cache after first access, hiding any disk-mode behavior. Deep8M (2.9 GB) is the smallest Deep variant big enough that page cache pressure becomes visible on a typical dev machine.
3. **No cache protocol**: v4 ran "warm" implicitly. For v5 we formalize **WARM as the primary steady-state protocol** because sudo is unavailable on the test machine, ruling out `drop_caches`. The paper's cold-I/O argument instead leans on bench_e2e's internal `avg_io_wait_ms` counter, which measures the actual I/O submit→complete time regardless of page cache state.

The authoritative experiment plan for this change is `refine-logs/BASELINE_PLAN_CN.md` v5, written 2026-04-11.

## Goals / Non-Goals

**Goals:**
- Produce FAISS OnDiskInvertedLists baseline numbers for Deep1M, Deep8M, COCO 100K under the **WARM steady-state protocol** (primary)
- Produce DiskANN disk-mode baseline numbers for the same three datasets
- Opt-in **semi-cold via `posix_fadvise(DONTNEED)`** as a best-effort auxiliary measurement (not required, not blocking)
- Produce E2E comparison table for COCO 100K combining {FAISS-disk, DiskANN-disk} × {FlatStor, Lance} vs BoundFetch integrated
- Preserve v4 pure-memory data as an appendix reference (relabel, don't delete)
- Report bench_e2e's internal `avg_io_wait_ms` alongside BoundFetch's WARM steady-state latency — this is the paper's authoritative "disk I/O cost" signal

**Non-Goals:**
- Not re-running v4 payload benchmarks (Deep1M/COCO already covered). Only Deep8M payload is new, and even that only uses `entity_id` (8 bytes) as payload — no synthetic 10KB blobs.
- Not building a large synthetic payload for Deep8M. Disk-mode vector search is the whole point for Deep8M; payload I/O is tested separately on COCO 100K.
- Not implementing BoundFetch itself on Deep8M in this change — that's a separate change.
- Not attempting to use `diskannpy 0.4.0` again in the old `lab` env. v4 already proved that was a dead end.
- Not testing HNSW (hnswlib) — it's a pure memory library, and we already have FAISS IVF-Flat as a memory upper bound in v4.

## Decisions

### D1: FAISS `OnDiskInvertedLists` over mmap
FAISS has three "disk" modes:
1. `read_index(path, faiss.IO_FLAG_MMAP)` — whole-file mmap, OS manages pages
2. `OnDiskInvertedLists` — explicit disk format, inverted lists read via pread on demand
3. `OnDiskIOProcessor` — custom IO handler (very complex)

**Chose (2)**. Rationale:
- mmap's page cache behavior is uncontrollable: a single warmup query loads the whole file into RAM, and subsequent queries are memory-hit. This defeats the cold protocol.
- OnDiskInvertedLists explicitly pread's each probed cluster's inverted list, matching BoundFetch's "probe cluster → pread disk" architecture. This is the most structurally similar baseline.
- The FAISS API is stable and well-documented in the FAISS wiki.

**Alternatives considered:**
- mmap: rejected because `drop_caches` is the only way to ensure cold reads, and even then the OS will prefetch aggressively
- Building a custom IO processor: rejected as unnecessary complexity for a baseline

### D2: Deep8M with nlist=12800
Standard heuristic `nlist ≈ 4 × sqrt(N)` gives ~11313 for 8M. Rounding up to 12800 (= 2^7 × 100) gives ~625 vectors/cluster, matching Deep1M/nlist=4096's ~244 vectors/cluster in the same order of magnitude. The user's decision was explicit: "deep8m的聚类数量改成12800".

### D3: WARM steady-state is the primary protocol; sudo-based cold is out of scope
Sudo is unavailable on the test machine, so `drop_caches` and `cgroup memory limits` are ruled out. The primary test protocol is:
- 100 warmup queries (latencies discarded)
- 1000 measurement queries (latencies recorded as avg/p50/p99)

This measures steady-state performance. Since all datasets (especially after the first warmup pass) end up in page cache, the reported latency is the "best case" I/O scenario for every baseline. **That is actually fine for the paper argument**: we argue "even in steady state (most favorable to the baselines), BoundFetch is competitive, and its internal `avg_io_wait_ms` counter shows that disk I/O is still a meaningful cost".

**Auxiliary protocol — SEMI-COLD via `posix_fadvise(DONTNEED)`**: For each measurement run, before the warmup phase, open each index file, call `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`, close it. This is a best-effort unprivileged hint to the kernel to evict that file's pages. It does not guarantee eviction (especially if another process has the file mapped), so the resulting number is labeled `semi_cold`, not `cold`. Used only as a sanity check, not as the primary metric.

**Why we drop the strict COLD protocol entirely**:
- Without `drop_caches`, we cannot guarantee a true cold state — any semi-cold run that shows WARM-like latency could simply mean `posix_fadvise` was ignored
- Reporting "COLD numbers" that turn out to be indistinguishable from WARM would mislead readers
- The honest story is: "All baselines measured in WARM steady state; BoundFetch's internal io_wait counter shows the residual disk cost"

**Alternatives considered and rejected:**
- Memory pressure trick (allocate a huge numpy array to evict page cache): crude, slow, unreliable — and FAISS may re-fault the index back into cache before the measurement loop starts
- cgroup memory limit to force eviction: requires root or careful cgroup v2 setup, which we don't have
- Run under `systemd-run --user --property=MemoryMax=...`: only works on systems with user cgroup delegation enabled, non-portable

### D4: Query count fixed at 1000, stored as deterministic indices
Both `deep8m_query.fvecs` (10K) and COCO 100K have more than 1000 queries. Sampling 1000 evenly-spaced indices and caching the sampled indices to `results/{dataset}_query1000_idx.npy` ensures every run uses the identical query set — critical for reproducibility and cross-run comparison.

### D5: BoundFetch integrated E2E reference comes from a fresh bench_e2e WARM run
`bench_e2e` already supports COCO 100K E2E. For this baseline change we re-run it under the same WARM steady-state protocol as the other baselines. The resulting `results.json` provides:
- `avg_query_time_ms` — matched apples-to-apples with WARM FAISS/DiskANN latency
- `pipeline_stats.avg_io_wait_ms` — the authoritative disk-I/O cost, independent of cache state
- `avg_rerank_cpu_ms` — from v4 Step 1

The paper's main argument becomes: "in WARM steady state, the io_wait counter is still X ms, which equals Y% of the E2E latency; BoundFetch is designed to overlap this I/O with compute, which is what enables the overall latency advantage".

### D6: CSV schema extension — add `protocol` column
The existing `vector_search.csv` and `payload_retrieval.csv` schemas do NOT have a `protocol` column. We add this column with values from `{memory, warm, semi_cold}`:
- `memory`: legacy v4 rows (pure-memory FAISS, no disk mode)
- `warm`: v5 primary protocol (100 warmup + 1000 measurement)
- `semi_cold`: v5 auxiliary protocol (`posix_fadvise` + 1000 measurement)

A migration script `migrate_v4_rows.py` handles the backfill in one shot.

### D7: diskannpy fallback path
If diskannpy 0.7+ also fails (unlikely but possible), the fallback is to directly call DiskANN's C++ binaries (`build_disk_index` and `search_disk_index` under the DiskANN source tree) via `subprocess`. The binaries are well-tested and never hit the Python binding issues of 0.4.0. This is documented in tasks.md as a contingency.

## Risks / Trade-offs

- **[R1] diskannpy 0.7 may still have surprising behavior on Python 3.12**: Mitigated by (a) running a sanity toy dataset first, (b) having the C++ binary fallback ready.
- **[R2] `posix_fadvise(DONTNEED)` may not drop pages if FAISS keeps fd open or mmaps the file**: This is why `semi_cold` is labeled as "best effort" and not treated as authoritative. If semi_cold latency ≈ warm latency, we simply document that fact and rely on bench_e2e's internal `avg_io_wait_ms` as the I/O cost signal instead. No fallback to sudo.
- **[R3] Deep8M training with nlist=12800 may take > 10 min for OnDiskInvertedLists**: Mitigated by caching the trained index to a `.index` file and reusing across runs. Training once; searching many times.
- **[R4] semi_cold and warm numbers are indistinguishable**: Acceptable outcome — it just means `posix_fadvise` was ignored. Document it in analysis.md and proceed with WARM-only comparison. The paper's I/O argument shifts to bench_e2e's `avg_io_wait_ms` counter rather than external cold simulation.
- **[R5] E2E linear-sum is not how BoundFetch actually runs**: Yes — BoundFetch's E2E has I/O-compute overlap, so `vec_search + payload` linear sum for baselines OVERSTATES their latency. This is actually GOOD for the paper: it's the most charitable interpretation of the baselines, and BoundFetch's integrated number is still expected to beat it.
- **[R6] Scope creep — the user may ask to add hnswlib, Milvus, Qdrant later**: Out of scope for this change. Keep v5 focused on "disk-mode FAISS + DiskANN + Deep8M", and add more systems only in a subsequent change if the reviewer explicitly asks.
- **[R7] Drift between v4 and v5 CSV schemas**: The `migrate_v4_rows.py` script is a one-time migration and must be run before the first v5 write. Forgetting it will mix schemas. Mitigation: the v5 write path should check for the `protocol` column and fail loudly if missing.

## Open Questions

- Will we need a `--threads` flag per benchmark to control parallelism? Currently v4 is single-threaded; single-threaded is more honest for per-query latency measurement, so default to `num_threads=1` for search.
- Is it worth measuring `iowait` directly via `/proc/$pid/stat` during the cold run? Would add a solid "X% of time was spent in disk I/O" number, strongly supporting the cold protocol's validity.
- The E2E comparison for Deep8M is not in scope — should we reconsider if the user's paper needs E2E data on larger datasets? If yes, that's a follow-up change.
