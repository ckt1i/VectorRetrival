#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace index {

// ============================================================================
// ConANN — Continuous Approximate Nearest Neighbor 3-class classifier
// ============================================================================

/// ConANN performs a three-way classification of candidate vectors based on
/// their approximate (RaBitQ) distance to the query vector.
///
/// The classification uses **additive thresholds** derived from a globally
/// precomputed reference distance d_k and the RaBitQ error bound ε:
///
///   - **SafeIn**:    approx_dist < d_k − 2ε   (definitely in top-K)
///   - **SafeOut**:   approx_dist > d_k + 2ε   (definitely not in top-K)
///   - **Uncertain**: otherwise                 (needs exact distance check)
///
/// where:
///   - d_k is a globally precomputed top-k reference distance, determined by
///     sampling the full dataset during build time.
///   - ε = c_factor · 2^(−bits/2) / √dim   (RaBitQ error bound)
///
/// This design makes the thresholds fixed (precomputed at build time) rather
/// than dynamic (no dependency on the runtime top-K heap state), which
/// simplifies the hot loop and enables early I/O scheduling decisions.
///
class ConANN {
 public:
    /// Construct a ConANN classifier with explicit epsilon and d_k values.
    ///
    /// @param epsilon  RaBitQ error bound: c · 2^(−B/2) / √D
    /// @param d_k      Precomputed global top-k reference distance
    ConANN(float epsilon, float d_k);

    /// Construct from RaBitQ config and a precomputed d_k.
    ///
    /// Computes epsilon from the config:
    ///   epsilon = cfg.c_factor * pow(2, -cfg.bits / 2.0f) / sqrt(dim)
    ///
    /// @param cfg  RaBitQ configuration (provides c_factor, bits)
    /// @param dim  Vector dimensionality
    /// @param d_k  Precomputed global top-k reference distance
    /// @return     ConANN instance
    static ConANN FromConfig(const RaBitQConfig& cfg, Dim dim, float d_k);

    /// Classify a candidate based on its approximate distance.
    ///
    /// @param approx_dist  Approximate squared L2 distance from RaBitQ
    /// @return             SafeIn / SafeOut / Uncertain
    ResultClass Classify(float approx_dist) const;

    /// Calibrate the global distance threshold d_k by sampling from the dataset.
    ///
    /// Algorithm:
    ///   1. Randomly sample `num_samples` vectors as pseudo-queries.
    ///   2. For each sample query, compute brute-force L2 distances to ALL
    ///      vectors in the dataset, find the top_k-th nearest distance.
    ///   3. Collect all sample top-k distances.
    ///   4. Sort and take the specified percentile as the global d_k.
    ///
    /// This d_k represents a "conservative" estimate of the top-k distance
    /// across the dataset, used as the center of the SafeIn/SafeOut interval.
    ///
    /// @param vectors     Row-major array of N vectors (N × dim floats)
    /// @param N           Number of vectors in the dataset
    /// @param dim         Vector dimensionality
    /// @param num_samples Number of pseudo-queries to sample (default 100)
    /// @param top_k       The k for top-k distance (default 10)
    /// @param percentile  Which percentile of sample d_k values to use (0–1,
    ///                    default 0.99). Higher = more conservative (fewer
    ///                    SafeOut, fewer missed true positives).
    /// @param seed        Random seed for reproducibility (0 = random)
    /// @return            Calibrated d_k value
    static float CalibrateDistanceThreshold(
        const float* vectors, uint32_t N, Dim dim,
        uint32_t num_samples = 100,
        uint32_t top_k = 10,
        float percentile = 0.99f,
        uint64_t seed = 42);

    /// Get the RaBitQ error bound epsilon.
    float epsilon() const { return epsilon_; }

    /// Get the precomputed global top-k reference distance.
    float d_k() const { return d_k_; }

    /// Get the SafeIn threshold: d_k − 2ε.
    float tau_in() const { return tau_in_; }

    /// Get the SafeOut threshold: d_k + 2ε.
    float tau_out() const { return tau_out_; }

 private:
    float epsilon_;    // ε = c · 2^(−B/2) / √D
    float d_k_;        // Global precomputed top-k reference distance
    float tau_in_;     // = d_k − 2ε
    float tau_out_;    // = d_k + 2ε
};

}  // namespace index
}  // namespace vdb
