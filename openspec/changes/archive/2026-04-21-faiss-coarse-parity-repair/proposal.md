## Why

The newly integrated in-process C++ `faiss_kmeans` coarse builder does not preserve the historical Python Faiss clustering behavior that previous experiments relied on. Diagnostics on `coco_100k / nlist=2048 / single` show a materially worse recall-vs-nprobe operating point, so the current integration is not yet safe to treat as a drop-in replacement.

## What Changes

- Replace the current hand-rolled `faiss::Clustering + IndexFlat` parity path with a builder implementation that directly reuses Faiss IVF coarse-training semantics, matching the historical Python exporter path.
- Align Faiss clustering parameters used by the C++ path with the historical exporter behavior, including effective metric handling, spherical clustering for IP/cosine semantics, training sample size, and iteration defaults.
- Make coarse probing metric-aware at query time so indexes built with cosine/IP-effective centroids are probed with the same effective metric rather than raw L2 only.
- Add explicit parity diagnostics and regression acceptance checks that compare the C++ builder path against the historical Python-precomputed Faiss artifacts before the new path is accepted as equivalent.

## Capabilities

### New Capabilities
- `faiss-coarse-parity`: Build and serve Faiss coarse partitions in-process while preserving the same coarse-training and probing semantics as the historical Python Faiss exporter path.

### Modified Capabilities
- `e2e-benchmark`: Benchmark outputs must distinguish Faiss coarse-builder source/metric provenance well enough to validate parity between historical precomputed artifacts and the new in-process builder path.

## Impact

- Affected code: `src/index/faiss_coarse_builder.cpp`, `src/index/ivf_builder.cpp`, `src/index/ivf_index.cpp`, `benchmarks/bench_e2e.cpp`, related metadata/tests.
- Affected behavior: `coarse_builder=faiss_kmeans` training path, coarse centroid assignment generation, coarse probing order during search, benchmark provenance.
- Dependencies/systems: vendored Faiss C++ integration, build metadata sidecars, regression scripts comparing Python-exported and in-process Faiss clustering.
