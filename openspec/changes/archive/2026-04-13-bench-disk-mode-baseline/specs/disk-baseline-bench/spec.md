## ADDED Requirements

### Requirement: Disk-mode FAISS baseline
The system SHALL provide a FAISS IVF-PQ baseline that stores inverted lists on disk via `faiss.OnDiskInvertedLists`, loads centroids and PQ codebook into RAM only, and produces latency/recall measurements for Deep1M, Deep8M, and COCO 100K datasets.

#### Scenario: Build FAISS disk index for Deep8M
- **WHEN** the user runs `run_faiss_ivfpq_disk.py --dataset deep8m --nlist 12800`
- **THEN** the script trains an `IndexIVFPQ` on a sample of Deep8M, calls `replace_invlists(faiss.OnDiskInvertedLists(...))`, persists the index to disk, and reports build time
- **AND** the resulting index file is reusable across subsequent search runs without retraining

#### Scenario: Search under WARM steady-state
- **WHEN** the user runs `run_faiss_ivfpq_disk.py --dataset deep8m --nprobe 128 --protocol warm --query-count 1000`
- **THEN** the script runs 100 warmup queries (latencies discarded), then runs 1000 measurement queries, recording per-query latency (avg/p50/p99)
- **AND** writes a row `{system=FAISS-IVFPQ-disk, dataset, nlist, nprobe, protocol=warm, recall@10, avg_latency_ms, ...}` to `results/vector_search.csv`

#### Scenario: Save Top-K fidx for payload bench
- **WHEN** running with `--save-ids` at the final nprobe value
- **THEN** the script saves `results/faiss_disk_topk_fidx_{dataset}_nprobe{N}.npy` with shape `[1000, 10]` containing 0-based FAISS vector indices
- **AND** for COCO 100K also saves `faiss_disk_topk_ids_coco_100k_nprobe{N}.npy` mapping faiss_idx → image_id

### Requirement: DiskANN disk-mode baseline
The system SHALL provide a DiskANN baseline running in proper disk mode via `diskannpy >= 0.7` (or the DiskANN C++ `search_disk_index` binary as fallback), with PQ compressed codes in RAM and full-precision vectors on SSD.

#### Scenario: Build DiskANN disk index for Deep8M
- **WHEN** the user runs `run_diskann_disk.py --dataset deep8m --R 64 --L 100`
- **THEN** the script calls `diskannpy.build_disk_index(...)` with `search_memory_maximum=1.0` (1 GB RAM budget) and `pq_disk_bytes=96` to force disk mode
- **AND** verifies that `{prefix}_disk.index` is produced and non-empty (not `_mem.index`)

#### Scenario: Search under WARM steady-state
- **WHEN** the user runs `run_diskann_disk.py --dataset deep8m --L_search 200 --protocol warm --query-count 1000`
- **THEN** the script loads the index via `StaticDiskIndex(...)`, runs 100 warmup queries, then 1000 measurement queries with `beam_width=4`, and records latency
- **AND** writes a row `{system=DiskANN-disk, dataset, R, L_build, L_search, protocol=warm, recall@10, ...}` to `results/vector_search.csv`

#### Scenario: Graceful fallback when diskannpy unavailable
- **WHEN** `import diskannpy` fails or `build_disk_index` is not available
- **THEN** the script prints the install instructions for `labnew` env (`pip install diskannpy>=0.7`) and exits with code 2
- **AND** does NOT write any fake row to the CSV

### Requirement: Deep8M dataset support
The system SHALL load Deep8M (8M × 96-dim float32) base vectors, queries, and ground truth from `/home/zcq/VDB/data/deep8m/`, supporting both `deep8m_base.fvecs` and `deep8m_base.bin` formats, and handling nlist=12800 for IVF clustering.

#### Scenario: Load Deep8M via fvecs
- **WHEN** a loader is called with `dataset=deep8m`
- **THEN** it reads `deep8m_base.fvecs`, `deep8m_query.fvecs`, and `deep8m_groundtruth.ivecs`
- **AND** returns arrays of shape `(8_000_000, 96)`, `(~10_000, 96)`, and `(~10_000, K)` respectively

#### Scenario: Sample 1000 queries from Deep8M query set
- **WHEN** a benchmark needs Q=1000 queries
- **THEN** it takes evenly-spaced indices from `deep8m_query.fvecs` and the matching ground truth rows
- **AND** caches the sampled indices to `results/deep8m_query1000_idx.npy` so subsequent runs use the identical query set

### Requirement: WARM primary protocol with semi-cold auxiliary
The system SHALL support a WARM steady-state protocol as the primary measurement mode (required), and a SEMI-COLD auxiliary protocol as best-effort (optional). The strict "drop_caches" COLD protocol is out of scope because sudo is unavailable on the test machine. A `--protocol` CLI flag selects between `warm` (default) and `semi_cold`.

#### Scenario: Warm protocol (primary)
- **WHEN** `--protocol warm` is selected (default)
- **THEN** the script runs 100 warmup queries first (latencies discarded), then runs the 1000 measurement queries
- **AND** the CSV row's `protocol` column reads `warm`
- **AND** this is the authoritative number used in the paper's primary comparison table

#### Scenario: Semi-cold protocol (auxiliary, unprivileged)
- **WHEN** `--protocol semi_cold` is selected
- **THEN** the script opens each target index file read-only, calls `os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)`, and closes the file before starting the measurement loop
- **AND** then runs the same 100 warmup + 1000 measurement queries (no separate cold-only loop)
- **AND** prints `[cache] requested DONTNEED on <path> (size N MB) — best-effort, kernel may ignore` to stderr
- **AND** the CSV row's `protocol` column reads `semi_cold`

#### Scenario: Semi-cold reporting caveat
- **WHEN** a semi_cold row and a warm row exist for the same (dataset, system, nprobe)
- **THEN** the analysis script computes `ratio = semi_cold_latency / warm_latency` and includes it in the auxiliary table
- **AND** if `ratio < 1.5`, annotates the row with `notes="posix_fadvise ineffective (ratio=X)"` — this is informational, not a failure

#### Scenario: sudo-based drop_caches is rejected
- **WHEN** the user passes `--use-sudo` or any sudo-related flag
- **THEN** the script prints `ERROR: sudo-based cache clearing is not supported in this change (no sudo on test machine)` and exits with code 2
- **AND** suggests using `--protocol warm` or `--protocol semi_cold` instead

### Requirement: In-memory v4 data preserved as appendix reference
The v4 pure-memory FAISS baseline results SHALL be preserved in `results/vector_search.csv` but labeled `notes="in-memory-reference-only"` and excluded from the primary comparison table in `results/analysis.md`.

#### Scenario: Relabel v4 rows
- **WHEN** the migration script `migrate_v4_rows.py` is run
- **THEN** all existing rows with `system IN (FAISS-IVF-PQ, FAISS-IVFPQ, FAISS-IVFFLAT)` and no `protocol` column get their `notes` field set to `"in-memory-reference-only"`
- **AND** the CSV gains a new column `protocol` with values `{memory, warm, semi_cold}`; all legacy v4 rows default to `memory`

### Requirement: E2E comparison scoped to COCO 100K
The E2E comparison table SHALL only cover COCO 100K and SHALL combine the WARM-protocol vector search and payload results from B1/B2/B5 into linearly-summed E2E latency, alongside the BoundFetch integrated E2E result (also WARM) and its internal `avg_io_wait_ms` counter for I/O cost attribution.

#### Scenario: Generate E2E table
- **WHEN** the user runs `run_e2e_comparison.py --dataset coco_100k --protocol warm --boundfetch-result <path>`
- **THEN** the script joins `vector_search.csv` (filtered by `dataset=coco_100k` AND `protocol=warm`) with `payload_retrieval.csv` (filtered same way), computes `e2e_ms = vec_search_ms + payload_ms`, and appends a BoundFetch reference row from the provided JSON including its `avg_io_wait_ms`
- **AND** produces `results/e2e_comparison.csv` sorted by `e2e_ms`
- **AND** the BoundFetch row includes a notes column documenting `avg_io_wait_ms=X` so the reader sees the residual disk cost

### Requirement: labnew Python 3.11 environment
All new scripts SHALL run under `/home/zcq/anaconda3/envs/labnew/bin/python` (Python 3.11). The scripts SHALL NOT rely on Python 2 syntax, the old `lab` env, or packages known to fail on 3.11. The labnew environment requires: `python==3.11.x`, `numpy==1.25`, `setuptools>=59.6`, `pybind11>=2.10.0`, `cmake>=3.22`, and the standard packages (`faiss-cpu>=1.8`, `diskannpy>=0.7`, `lancedb`, `pyarrow`).

#### Scenario: Environment sanity check
- **WHEN** `check_labnew.py` runs
- **THEN** it verifies Python version is 3.11.x, imports faiss and checks version ≥ 1.8, imports diskannpy and checks version ≥ 0.7, imports lancedb, pyarrow, and checks build dependencies (numpy==1.25, setuptools>=59.6, pybind11>=2.10.0, cmake>=3.22)
- **AND** exits with code 0 if all checks pass; code 1 otherwise with a list of missing/wrong packages
