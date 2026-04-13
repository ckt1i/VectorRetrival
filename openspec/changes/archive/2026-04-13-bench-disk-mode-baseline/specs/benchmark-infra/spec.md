## ADDED Requirements

### Requirement: Cache-clearing helper module
The benchmark infrastructure SHALL provide a shared Python helper `_cache_utils.py` under `/home/zcq/VDB/baselines/` that implements `drop_file_cache(path: str)` using `posix_fadvise(DONTNEED)` and `drop_global_cache()` shelling out to `sync && echo 3 > /proc/sys/vm/drop_caches` with sudo, so any baseline script can import it consistently.

#### Scenario: Import and use drop_file_cache
- **WHEN** a baseline script imports `from _cache_utils import drop_file_cache` and calls it on an index file path
- **THEN** the helper opens the file in read-only mode, calls `os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)`, and logs the size dropped
- **AND** returns without raising even if the file does not exist (logs a warning instead)

### Requirement: Disk-mode dataset loaders
The benchmark infrastructure SHALL support loading Deep8M base/query/ground-truth files in both `.fvecs` and `.bin` formats from `/home/zcq/VDB/data/deep8m/`, and SHALL expose a uniform interface `load_dataset(name, split, max_queries)` returning numpy arrays.

#### Scenario: Load Deep8M with downsampled queries
- **WHEN** calling `load_dataset("deep8m", split="query", max_queries=1000)`
- **THEN** the helper returns `(queries[1000, 96], gt[1000, K])` using evenly-spaced indices from the full 10K query set
- **AND** caches the sampled indices to disk so repeated calls return identical queries
