## Context

The repository now vendors Faiss C++ and exposes `coarse_builder=faiss_kmeans` as an in-process clustering path. However, diagnostics on `coco_100k / nlist=2048 / single` show that the current implementation materially underperforms the historical Python-generated Faiss artifacts that earlier experiments used as the reference path.

Two failure modes are now explicit:

- The current C++ adapter does not reproduce historical Python Faiss coarse-training semantics. It uses a hand-rolled `faiss::Clustering + IndexFlat` flow instead of the IVF coarse-training path used by the Python exporter.
- Query-time coarse probing still ranks centroids with raw L2 in `IvfIndex::FindNearestClusters()`, even when the builder metadata says the effective coarse metric is IP/cosine.

Because both builder-time and query-time semantics drifted, the resulting recall-vs-nprobe curve is not a safe replacement for the historical Faiss clustering workflow.

## Goals / Non-Goals

**Goals:**
- Reproduce the historical Python Faiss coarse-builder path in-process in C++, using Faiss IVF coarse-training semantics rather than an approximate reimplementation.
- Make query-time coarse probing respect the same effective metric that the builder used to train coarse centroids.
- Preserve the current project structure: Faiss remains a local coarse-builder dependency, not a search-stack rewrite.
- Add provenance and regression hooks strong enough to validate parity between Python-precomputed artifacts and the in-process C++ path.

**Non-Goals:**
- Re-optimizing SuperKMeans or hierarchical clustering in this change.
- Introducing GPU Faiss or changing the main project's C++17 boundary.
- Claiming bit-identical clustering artifacts across environments.
- Changing downstream rerank, CRC, or storage formats beyond the metadata needed for parity tracking.

## Decisions

### 1. The Faiss parity path SHALL reuse `IndexIVFFlat` coarse training semantics

The primary repair is to stop treating `faiss::Clustering` as the parity implementation. The historical Python exporter creates `IVF<nlist>,Flat`, trains that index, reconstructs quantizer centroids, and uses the quantizer itself for 1-NN assignments. The C++ parity path should mirror that structure:

- create `IndexFlatIP` or `IndexFlatL2` quantizer
- create `IndexIVFFlat`
- configure IVF coarse-training parameters directly on that object
- call `train()`
- extract centroids from `quantizer`
- derive assignments with `quantizer->search(..., 1, ...)`

Why this over patching the existing adapter:
- It directly follows the historical reference path.
- It inherits Faiss IVF defaults such as inner-product spherical clustering behavior.
- It reduces the number of hidden semantic mismatches that must be guessed and manually replicated.

Alternative considered:
- Keep `faiss::Clustering` and add missing flags such as `spherical`.
Why rejected:
- The diagnosis already shows the current hand-rolled path drifted materially from the Python reference.
- Even after patching obvious flags, it would still remain a reimplementation rather than direct reuse of the reference training path.

### 2. Coarse metric handling SHALL be split into requested metric and effective coarse metric

The builder already carries the concept of requested metric (`cosine`, `ip`, `l2`) and effective metric (`ip` or `l2`). This separation must become operational rather than informational:

- `cosine` means normalize training/query vectors for coarse logic and use Faiss IP semantics
- `ip` means use IP semantics without cosine normalization
- `l2` means use L2 semantics without normalization

Why:
- The current metadata already records this split, so the missing piece is enforcement.
- It gives a single source of truth for both builder-time and query-time coarse ranking.

Alternative considered:
- Infer coarse metric ad hoc from `coarse_builder`.
Why rejected:
- The same builder can operate under multiple metric semantics.
- Metric behavior belongs to index provenance, not builder identity.

### 3. `FindNearestClusters()` SHALL become metric-aware for Faiss-built indexes

For indexes whose effective coarse metric is IP, query-time centroid ranking must no longer use raw L2 over raw centroids. Instead:

- load effective coarse metric from build provenance during `IvfIndex::Open()`
- for IP-effective indexes, rank centroids by inner product
- apply the same normalization rule used at build time when the requested metric is cosine

Why this over keeping query-time L2:
- Diagnostics show large candidate-cover differences between raw-L2 probe ordering and IP/cosine-consistent ordering on the same centroid set.
- Builder parity alone is insufficient if probing continues to visit the wrong centroid order.

Alternative considered:
- Leave search semantics unchanged and only align builder artifacts.
Why rejected:
- It would knowingly preserve a probe-time semantic mismatch.
- That would keep the final recall-vs-nprobe curve below the historical Faiss path even if training parity improved.

### 4. Parity acceptance SHALL be judged in two layers

The repaired path should not be accepted based on final recall alone. Validation should explicitly separate:

- artifact-level parity: centroids/assignments close enough to the historical Python path
- operating-point parity: same coarse cover / recall-vs-nprobe decision tier

Why:
- This isolates whether a regression comes from builder semantics or query-time probing.
- It avoids another round of opaque “Faiss path got worse” debugging.

Alternative considered:
- Only compare final `recall@10`.
Why rejected:
- Final recall conflates builder drift and probe drift.
- That hides the actual fault line and slows follow-up diagnosis.

## Risks / Trade-offs

- [Faiss API parity still differs across versions] → Pin the repair to the vendored Faiss version already in `thrid-party/faiss` and document the exact training defaults used.
- [Metric-aware probing changes behavior for existing Faiss-built indexes] → Gate the behavior on stored build provenance, not only on `coarse_builder` name.
- [Artifact parity may still not be bit-identical] → Accept “same operating-point tier” rather than exact centroid equality, but require much stronger similarity than the current drift.
- [Benchmark CSV/source fields remain misleading] → Update provenance outputs together with the parity repair so regression results cannot be misread.

## Migration Plan

1. Replace the current Faiss coarse-builder internals with an `IndexIVFFlat`-based parity path while preserving the same public `coarse_builder=faiss_kmeans` interface.
2. Extend `IvfIndex` loading logic so build provenance captures effective coarse metric and query-time centroid ranking follows it.
3. Re-run focused parity checks against the historical Python-generated Faiss artifacts.
4. Only after parity is confirmed, treat the in-process C++ path as the default Faiss coarse-builder implementation for experiments.

Rollback is straightforward: retain support for precomputed Faiss artifacts and continue loading them if the in-process parity path fails acceptance.

## Open Questions

- Whether the first repair pass should expose `faiss_niter=10` as the public default immediately or only hard-code it internally for parity mode.
- Whether centroid/query normalization for IP-effective probing should be cached eagerly on index open or computed lazily.
- Whether artifact-level parity should be encoded as an automated regression test or remain an experiment harness check in the first pass.
