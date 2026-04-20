## 1. Builder Parity

- [x] 1.1 Replace the current `faiss::Clustering + IndexFlat` coarse-builder path with an `IndexIVFFlat`-based Faiss parity path that mirrors the historical Python exporter flow
- [x] 1.2 Align Faiss coarse-training defaults for the parity path, including effective metric handling, spherical clustering for IP-effective builds, train size, iteration count, and restart count
- [x] 1.3 Derive centroids and primary assignments from the trained Faiss quantizer and preserve existing precomputed import behavior without changing the external `coarse_builder=faiss_kmeans` interface

## 2. Metric-Aware Probing

- [x] 2.1 Extend index-open/build provenance handling so `IvfIndex` can load and store the effective coarse metric required for query-time probing
- [x] 2.2 Update `FindNearestClusters()` so Faiss-built indexes probe centroids with metric-consistent ranking, including cosine/IP handling and L2 fallback
- [x] 2.3 Keep benchmark/config outputs consistent with the true Faiss builder source and coarse metric so regression results cannot be mislabeled

## 3. Parity Validation

- [x] 3.1 Add focused tests for Faiss parity behavior covering in-process build metadata, effective metric loading, and metric-aware centroid probing
- [x] 3.2 Add or update regression harness support to compare in-process C++ Faiss clustering against historical Python-precomputed Faiss artifacts under the same benchmark family
- [x] 3.3 Re-run the `coco_100k / nlist=2048 / topk=10 / single` Faiss parity check and verify the repaired path returns to the same recall-vs-nprobe operating-point tier as the historical Python path
