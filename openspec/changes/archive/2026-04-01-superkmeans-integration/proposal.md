# Proposal: SuperKMeans Integration + Third-Party Cleanup

## Summary

Replace the hand-written KMeans in ivf_builder and benchmarks with SuperKMeans library. Remove the capacity-constrained balance feature. Clean up third-party dependencies.

## Problem

1. **Slow clustering**: ivf_builder's KMeans is single-threaded O(N×K×D) per iteration. For high-dimensional data (D=512+), SuperKMeans with PDX pruning is 6x+ faster.
2. **Redundant code**: The same naive KMeans is copy-pasted across 6 benchmark files (~50 lines each).
3. **Messy third-party**: `conann/` is never used, `Extended-RaBitQ/` is reference-only, Eigen is duplicated.

## Changes

1. **CMake**: `add_subdirectory(thrid-party/SuperKMeans)`, link to `vdb_index` and benchmarks
2. **ivf_builder**: Replace `RunKMeans()` with SuperKMeans or precomputed file loading
3. **Remove balance**: Delete `balance_factor` config, capacity-constrained reassignment code, and related tests
4. **Benchmarks**: Delete static `KMeans()`, use SuperKMeans or `--centroids`/`--assignments`
5. **Third-party cleanup**: Delete `conann/`, keep `Extended-RaBitQ/` as reference (not in build)

## Balance Removal Justification

- SuperKMeans doesn't support capacity constraints
- The balance feature adds ~80 lines of complexity in ivf_builder
- In practice, standard KMeans with good initialization (KMeans++ or SuperKMeans) produces reasonably balanced clusters
- If extreme imbalance occurs, it's better addressed by hierarchical clustering (which SuperKMeans supports via `HierarchicalSuperKMeans`)

## Scope

### In scope
- CMake: add SuperKMeans, remove conann
- ivf_builder.cpp: replace RunKMeans, remove balance
- ivf_builder.h / IvfBuildConfig: remove balance_factor
- All 6 benchmarks: delete static KMeans(), use SuperKMeans
- Tests: update/remove balance-related tests
- thrid-party/conann: delete

### Out of scope
- overlap_scheduler (doesn't do clustering)
- Storage format changes (separate change)

## Success Criteria

- All unit tests pass (minus removed balance tests)
- ivf_builder produces valid index with SuperKMeans
- Benchmarks run without static KMeans function
- COCO100k (D=512) clustering < 10 sec (vs current minutes)
