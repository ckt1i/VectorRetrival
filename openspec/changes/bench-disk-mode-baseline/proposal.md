## Why

The previous baseline change (`bench-e2e-rerank-timing-and-baseline`) established pure-memory FAISS IVF-PQ/IVF-Flat baselines and payload retrieval (FlatStor/Lance/Parquet) benchmarks. However, BoundFetch is a **disk-based** vector search + payload system whose core claim is I/O–compute overlap under cold-cache conditions. A pure-memory FAISS comparison is apples-to-oranges: reviewers will reject "BoundFetch (disk) vs FAISS (memory)" as an unfair setup.

This change introduces disk-mode baselines — FAISS `OnDiskInvertedLists` and DiskANN in real disk mode — and adds **Deep8M** (8M × 96-dim, 2.9 GB) as a larger dataset that won't be fully served from page cache. Together with a COLD vs WARM cache test protocol, this produces baseline numbers that directly support the paper claim "C6: BoundFetch outperforms existing disk-based systems in search+payload E2E."

A new Python 3.11 conda environment (`labnew`) is available, unblocking `diskannpy >= 0.7` which has the correct disk-mode API and fixed memory cleanup bugs that made `diskannpy 0.4.0` unusable in the old `lab` env. Python 3.11 is required due to diskannpy's dependency on numpy==1.25 (incompatible with Python 3.12).

## What Changes

- Add Layer-1 disk-mode vector search baselines:
  - **FAISS IVF-PQ + OnDiskInvertedLists** — FAISS official disk format; inverted lists on disk, centroids/codebook in RAM
  - **DiskANN disk mode** via `diskannpy >= 0.7` — graph + PQ compressed on SSD, beam search with explicit disk I/O
- Add Deep8M (8M × 96-dim) as the primary "disk pressure" dataset with `nlist=12800`
- Keep Deep1M and COCO 100K as secondary reference datasets
- Introduce **WARM steady-state protocol as primary** — 100 warmup queries + 1000 measurement queries; `posix_fadvise(DONTNEED)` serves as an opt-in best-effort semi-cold check, **not** a strict cold protocol (sudo is unavailable on the test machine, so `drop_caches` is out of scope)
- Rely on **bench_e2e's internal `avg_io_wait_ms`** as the authoritative "disk I/O cost" signal for BoundFetch — this is measured from I/O submit→complete inside the system, independent of OS page cache state
- Switch all Python execution to `/home/zcq/anaconda3/envs/labnew/bin/python`
- E2E comparison scope narrows to **COCO 100K only** (Deep1M/Deep8M are Layer-1 only, since we don't want to build large synthetic payloads for them)
- **BREAKING**: Previous v4 pure-memory baseline results are preserved but relabeled as `notes="in-memory-reference-only"` in `results/vector_search.csv` and move to an appendix table in `results/analysis.md`

## Capabilities

### New Capabilities
- `disk-baseline-bench`: Disk-mode baseline benchmarking with FAISS OnDiskInvertedLists, DiskANN disk mode, Deep8M dataset, and COLD/WARM cache protocol. Produces CSVs and analysis artifacts that support paper claim C6.

### Modified Capabilities
- `benchmark-infra`: Extended to support disk-mode index builds, cache-clearing helpers (`drop_caches`, `posix_fadvise`), and Deep8M data loaders.

## Impact

- **Code**: New scripts under `/home/zcq/VDB/baselines/vector_search/`:
  - `run_faiss_ivfpq_disk.py` (FAISS OnDiskInvertedLists)
  - `run_diskann_disk.py` (DiskANN proper disk mode)
  - New helper `_cache_utils.py` for drop_caches / posix_fadvise
- **Data**: New Deep8M index files under `/home/zcq/VDB/baselines/data/deep8m_*/`; COCO 100K groundtruth precomputed and cached to `/home/zcq/VDB/data/coco_100k/groundtruth_top10.npy`
- **Environment**: All new runs use `labnew` (Python 3.11) conda env; requires `python==3.11.x`, `numpy==1.25`, `setuptools>=59.6`, `pybind11>=2.10.0`, `cmake>=3.22`, `faiss-cpu>=1.8`, `diskannpy>=0.7`, `lancedb`, `pyarrow`
- **Permissions**: No sudo available. All cache-clearing uses unprivileged `posix_fadvise(DONTNEED)`; whole-system `drop_caches` is not used. Honesty about this is preserved in the final analysis.
- **Outputs**: Extends `/home/zcq/VDB/baselines/results/vector_search.csv`, `payload_retrieval.csv`, `e2e_comparison.csv`; updates `analysis.md` with COLD/WARM tables
- **Docs**: Updates `refine-logs/EXPERIMENT_TRACKER_CN.md` Phase 0 section; BASELINE_PLAN_CN.md v5 is the authoritative plan for this change
