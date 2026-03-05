#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"

namespace vdb {
namespace rabitq {

// ============================================================================
// PreparedQuery — precomputed per-query data
// ============================================================================

/// Precomputed data for a query vector, reusable across all database codes.
///
/// For each query q and cluster centroid c:
///   1. Compute residual: r = q - c
///   2. Compute ‖q - c‖₂
///   3. Normalize: q̄ = r / ‖r‖
///   4. Rotate: q' = P^T × q̄
///   5. Sign-quantize q' → query sign code (for XOR+popcount fast path)
///   6. Precompute sum_q = Σ q'[i]  (needed in distance formula)
///
struct PreparedQuery {
    std::vector<float>    rotated;     // q' = P^T × q̄  (length = dim)
    std::vector<uint64_t> sign_code;   // Sign bits of q' packed as uint64_t
    float                 norm_qc;     // ‖q - c‖₂
    float                 norm_qc_sq;  // ‖q - c‖₂²
    float                 sum_q;       // Σ q'[i] (sum of all rotated components)
    Dim                   dim;         // Vector dimensionality
    uint32_t              num_words;   // ceil(dim / 64)
};

// ============================================================================
// RaBitQEstimator — approximate distance computation
// ============================================================================

/// Approximate distance estimator for RaBitQ 1-bit quantized vectors.
///
/// The core formula for L2 squared distance estimation:
///
///   ‖o - q‖² ≈ ‖o-c‖² + ‖q-c‖² - 2·‖o-c‖·‖q-c‖·⟨q̄, ô⟩
///
/// where the inner product ⟨q̄, ô⟩ is estimated using the 1-bit codes:
///
///   ⟨q̄, ô⟩ ≈ (2/√L)·dot(q', x) - (1/√L)·Σq'ᵢ
///
/// The dot product dot(q', x) can be computed two ways:
///
///   **Sign-code path (fast):**
///     dot(q', x) = Σ_x[i]=1 q'[i]
///                = Σ|q'[i]| - 2·Σ_{q_sign≠x} |q'[i]|
///     Approximation using popcount:
///       = (L - 2·popcount(q_sign XOR x)) × (1/√L)  ... simplified
///     But more precisely for 1-bit RaBitQ:
///       dot(q', x) ≈ (sum_x - 2·hamming(q_sign, x)) × factor
///     This path trades accuracy for speed.
///
///   **Float path (accurate):**
///     Direct Σ q'[i] × (2·x[i] - 1) using the rotated query.
///     Equivalent to scanning each bit of x and accumulating ±q'[i].
///
/// This estimator implements both paths. The fast path uses XOR+popcount
/// and is O(L/64) per vector; the accurate path is O(L) per vector.
///
class RaBitQEstimator {
 public:
    /// Construct an estimator for the given dimensionality.
    explicit RaBitQEstimator(Dim dim);

    ~RaBitQEstimator() = default;
    VDB_DISALLOW_COPY_AND_MOVE(RaBitQEstimator);

    /// Prepare a query vector for distance estimation.
    ///
    /// This precomputes all per-query data so that subsequent calls to
    /// EstimateDistance are efficient (only per-vector work remains).
    ///
    /// @param query     Raw query vector (length = dim)
    /// @param centroid  Cluster centroid (length = dim, or nullptr for zero)
    /// @param rotation  Rotation matrix (same as used for encoding)
    /// @return          Prepared query structure
    PreparedQuery PrepareQuery(const float* query,
                               const float* centroid,
                               const RotationMatrix& rotation) const;

    /// Estimate the squared L2 distance between a prepared query and a
    /// database code using the fast XOR+popcount path.
    ///
    /// Formula:
    ///   ip_est = (2/√L) × (sum_x - 2 × hamming) - (1/√L) × sum_q
    ///   dist² ≈ norm_oc² + norm_qc² - 2 × norm_oc × norm_qc × ip_est
    ///
    /// where hamming = popcount(q_sign XOR db_code).
    ///
    /// @param pq    Prepared query (from PrepareQuery)
    /// @param code  Database vector's RaBitQ code
    /// @return      Estimated ‖o - q‖² (may be negative due to approximation;
    ///              caller should clamp to 0 if needed)
    float EstimateDistance(const PreparedQuery& pq,
                           const RaBitQCode& code) const;

    /// Estimate distance using the more accurate float-dot path.
    ///
    /// Scans each bit of the database code and accumulates q'[i] × (2·x[i]-1)
    /// to compute the inner product exactly (no popcount approximation).
    /// This is O(L) per vector instead of O(L/64).
    ///
    /// @param pq    Prepared query
    /// @param code  Database vector's RaBitQ code
    /// @return      Estimated ‖o - q‖²
    float EstimateDistanceAccurate(const PreparedQuery& pq,
                                    const RaBitQCode& code) const;

    /// Batch estimate: compute estimated distances for multiple database codes.
    ///
    /// @param pq       Prepared query
    /// @param codes    Array of database codes (size = n)
    /// @param n        Number of codes
    /// @param out_dist Output distances (at least n elements)
    void EstimateDistanceBatch(const PreparedQuery& pq,
                                const RaBitQCode* codes,
                                uint32_t n,
                                float* out_dist) const;

    Dim dim() const { return dim_; }

 private:
    Dim dim_;
    uint32_t num_words_;  // ceil(dim / 64)
    float inv_sqrt_dim_;  // 1 / √dim  (precomputed)
};

}  // namespace rabitq
}  // namespace vdb
