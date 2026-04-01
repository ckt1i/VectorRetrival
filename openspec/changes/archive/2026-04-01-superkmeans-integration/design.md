# Design: SuperKMeans Integration

## CMake Integration

```cmake
# In top-level CMakeLists.txt, before vdb_index target:
set(SKMEANS_COMPILE_TESTS OFF CACHE BOOL "" FORCE)
set(SKMEANS_COMPILE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SKMEANS_COMPILE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKMEANS_SKIP_FFTW ON CACHE BOOL "" FORCE)
add_subdirectory(thrid-party/SuperKMeans)

# Link to vdb_index (ivf_builder uses it)
target_link_libraries(vdb_index PUBLIC ... superkmeans)

# Benchmarks also link
target_link_libraries(bench_rabitq_accuracy PRIVATE ... superkmeans)
target_link_libraries(bench_vector_search PRIVATE ... superkmeans)
# etc.
```

SuperKMeans is a header-only INTERFACE library. It transitively brings:
- OpenMP (via find_package)
- OpenBLAS or MKL (via find_package)
- Eigen (bundled in extern/)

## IvfBuilder Changes

### Before (current)

```cpp
Status IvfBuilder::RunKMeans(const float* vectors, uint32_t N, Dim dim) {
    // 130 lines: KMeans++ init + iterative refinement + balance
}
```

### After

```cpp
Status IvfBuilder::RunKMeans(const float* vectors, uint32_t N, Dim dim) {
    const uint32_t K = config_.nlist;

    if (!config_.centroids_path.empty() && !config_.assignments_path.empty()) {
        // Path 1: Load precomputed
        return LoadPrecomputedClustering(config_.centroids_path,
                                         config_.assignments_path, N, dim);
    }

    // Path 2: SuperKMeans
    skmeans::SuperKMeansConfig skm_cfg;
    skm_cfg.iters = config_.max_iterations;
    skm_cfg.seed = config_.seed;
    skm_cfg.verbose = false;

    auto skm = skmeans::SuperKMeans(K, dim, skm_cfg);
    auto c = skm.Train(vectors, N);
    auto a = skm.Assign(vectors, c.data(), N, K);

    centroids_.assign(c.begin(), c.end());
    assignments_.assign(a.begin(), a.end());

    return Status::OK();
}
```

### IvfBuildConfig Changes

```cpp
struct IvfBuildConfig {
    uint32_t nlist = 256;
    uint32_t nprobe = 32;
    uint32_t max_iterations = 10;
    uint64_t seed = 42;
    // REMOVED: float balance_factor = 0.0f;
    // NEW:
    std::string centroids_path;      // optional precomputed centroids (.fvecs)
    std::string assignments_path;    // optional precomputed assignments (.ivecs)
    // ... rest unchanged
};
```

### Removed Code

- `balance_factor` field from IvfBuildConfig
- Capacity-constrained reassignment loop (~80 lines in RunKMeans)
- `balance_factor` parameter in segment metadata serialization
- Test `BalancedClustering` in ivf_builder_test.cpp

## Benchmark Cleanup

All 6 benchmarks currently have a copy-pasted `static void KMeans(...)` function. Replace with:

```cpp
#include "superkmeans/superkmeans.h"

// ... in main():
std::vector<float> centroids;
std::vector<uint32_t> assignments;

if (!centroids_path.empty() && !assignments_path.empty()) {
    // Load precomputed (existing code from exrabitq-alignment change)
    auto c_or = io::LoadVectors(centroids_path);
    auto a_or = io::LoadIvecs(assignments_path);
    nlist = c_or.value().rows;
    centroids.assign(c_or.value().data.begin(), c_or.value().data.end());
    assignments.resize(N);
    for (uint32_t i = 0; i < N; ++i)
        assignments[i] = static_cast<uint32_t>(a_or.value().data[i]);
} else {
    // SuperKMeans
    skmeans::SuperKMeansConfig skm_cfg;
    skm_cfg.iters = 10;
    skm_cfg.seed = 42;
    auto skm = skmeans::SuperKMeans(nlist, dim, skm_cfg);
    auto c = skm.Train(base_data, N);
    auto a = skm.Assign(base_data, c.data(), N, nlist);
    centroids.assign(c.begin(), c.end());
    assignments.assign(a.begin(), a.end());
}
```

Delete the `static void KMeans(...)` function from each file.

## Third-Party Cleanup

```
BEFORE:                              AFTER:
thrid-party/                         thrid-party/
├── conann/          ← DELETE        ├── SuperKMeans/     ← linked via CMake
├── Extended-RaBitQ/ ← keep          └── Extended-RaBitQ/ ← reference only
└── SuperKMeans/     ← integrate
```

Extended-RaBitQ stays for algorithm reference but is NOT in the CMake build tree.

## Eigen Handling

SuperKMeans bundles Eigen in `extern/Eigen/`. Since our core code doesn't use Eigen directly, there's no conflict. The SuperKMeans CMakeLists.txt handles this:

```cmake
# SuperKMeans CMakeLists.txt (line 139-151):
if(TARGET Eigen3::Eigen)
    # Use existing target
elseif(Eigen3_FOUND)
    # Use system Eigen
else()
    # Use bundled extern/Eigen
endif()
```

We need to ensure the bundled Eigen is populated. Currently it's a copy from Extended-RaBitQ. This should be committed or documented.

## OpenMP Consideration

SuperKMeans uses OpenMP for multi-threaded clustering. This adds `-fopenmp` to compilation flags for `vdb_index` and linked targets. Impact:

- **Build**: Requires libgomp (usually available on Linux)
- **Runtime**: OpenMP thread pool created on first use, idle otherwise
- **Query path**: Not affected (SuperKMeans only called during index build)
