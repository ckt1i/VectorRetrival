#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"

namespace vdb {
namespace index {

// ============================================================================
// IvfBuilderConfig — configuration for building an IVF index
// ============================================================================

/// Configuration for IvfBuilder.  All parameters for IVF clustering,
/// RaBitQ encoding, ConANN calibration, and on-disk layout.
struct IvfBuilderConfig {
    /// Number of IVF clusters (nlist).
    uint32_t nlist = 16;

    /// K-means maximum iterations.
    uint32_t max_iterations = 20;

    /// K-means convergence tolerance (relative centroid movement).
    float tolerance = 1e-4f;

    /// Random seed for K-means initialization and ConANN calibration.
    uint64_t seed = 42;

    /// Balance factor for capacity-constrained K-means.
    /// 0 = standard K-means (no balancing).
    /// 1 = strict capacity constraint (each cluster gets exactly N/K vectors).
    /// Values in (0, 1) interpolate between standard and strict.
    ///
    /// When > 0, a capacity-constrained reassignment is applied after each
    /// standard K-means iteration: clusters exceeding
    ///   max_cap = ceil(N * (1 + balance_factor) / nlist)
    /// have their farthest vectors moved to the nearest under-capacity cluster.
    float balance_factor = 0.0f;

    /// RaBitQ configuration for encoding.
    RaBitQConfig rabitq;

    /// Payload column schemas (empty = no payload).
    std::vector<ColumnSchema> payload_schemas;

    // ----- ConANN calibration -----
    /// Number of pseudo-query samples for d_k calibration.
    uint32_t calibration_samples = 100;

    /// Top-k used in d_k calibration.
    uint32_t calibration_topk = 10;

    /// Percentile for d_k calibration (0–1, e.g. 0.99).
    float calibration_percentile = 0.99f;

    /// Page size for DataFile alignment (bytes). 1 = no padding.
    uint32_t page_size = 1;

    /// Segment ID to embed in segment.meta.
    uint64_t segment_id = 0;

    /// Default nprobe stored in segment.meta.
    uint32_t nprobe = 1;

    /// Number of pseudo-query samples per cluster for ε_ip calibration.
    /// If cluster_size < 2 * epsilon_samples, uses max(cluster_size / 2, 1).
    uint32_t epsilon_samples = 20;

    /// Percentile for ε_ip calibration (0–1, e.g. 0.95).
    float epsilon_percentile = 0.95f;
};

// ============================================================================
// PayloadFn — callback to supply per-vector payload during Build
// ============================================================================

/// Callback to supply payload columns for a given vector index.
/// Returns a vector of Datum values matching config_.payload_schemas.
/// Return empty vector if no payload for this vector.
using PayloadFn = std::function<std::vector<Datum>(uint32_t vec_index)>;

// ============================================================================
// IvfBuilder — builds an IVF+RaBitQ index to disk
// ============================================================================

/// Builds a complete IVF index from raw float vectors.
///
/// Build phases:
///   1. **K-means clustering** (optionally capacity-constrained via
///      balance_factor).
///   2. **ConANN calibration** — sample-based d_k estimation.
///   3. **Per-cluster writing** — for each cluster:
///      a. RaBitQ encode all assigned vectors.
///      b. Write a .dat DataFile (raw vectors, optional payload).
///      c. Write a .clu ClusterStore (centroid, codes, norms, address column).
///   4. **Global metadata** — write centroids.bin, rotation.bin, segment.meta
///      (FlatBuffers).
///
/// Usage:
///   IvfBuilderConfig cfg;
///   cfg.nlist = 16;
///   IvfBuilder builder(cfg);
///   // vectors: row-major N × dim float array
///   Status s = builder.Build(vectors, N, dim, output_dir);
///
class IvfBuilder {
 public:
    explicit IvfBuilder(const IvfBuilderConfig& config);
    ~IvfBuilder();

    VDB_DISALLOW_COPY_AND_MOVE(IvfBuilder);

    /// Build the full IVF index from raw vectors and write to `output_dir`.
    ///
    /// @param vectors    Row-major array of N × dim floats
    /// @param N          Number of vectors
    /// @param dim        Vector dimensionality
    /// @param output_dir Output directory (will be created if missing)
    /// @param payload_fn Optional callback to supply per-vector payload.
    ///                   If nullptr, records are written with empty payload.
    /// @return Status
    Status Build(const float* vectors, uint32_t N, Dim dim,
                 const std::string& output_dir,
                 PayloadFn payload_fn = nullptr);

    /// Optional progress callback: called once per cluster written.
    /// Arguments: (cluster_index, total_clusters)
    using ProgressCallback = std::function<void(uint32_t, uint32_t)>;
    void SetProgressCallback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

    /// Get the assignments from the last Build() call  (length N).
    /// assignments[i] = cluster index [0, nlist) for vector i.
    const std::vector<uint32_t>& assignments() const { return assignments_; }

    /// Get the centroids from the last Build() call (nlist × dim row-major).
    const std::vector<float>& centroids() const { return centroids_; }

    /// Get the calibrated d_k from the last Build() call.
    float calibrated_dk() const { return calibrated_dk_; }

 private:
    // ----- Internal helpers -----

    /// Phase A: capacity-constrained K-means clustering.
    Status RunKMeans(const float* vectors, uint32_t N, Dim dim);

    /// Phase B: calibrate ConANN d_k by sampling.
    void CalibrateDk(const float* vectors, uint32_t N, Dim dim);

    /// Phase C+D: write per-cluster files + global metadata.
    Status WriteIndex(const float* vectors, uint32_t N, Dim dim,
                      const std::string& output_dir,
                      PayloadFn payload_fn);

    IvfBuilderConfig config_;
    std::vector<uint32_t> assignments_;  // vector → cluster index
    std::vector<float> centroids_;       // nlist × dim row-major
    float calibrated_dk_ = 0.0f;
    float calibrated_eps_ip_ = 0.0f;
    ProgressCallback progress_cb_;
};

}  // namespace index
}  // namespace vdb
