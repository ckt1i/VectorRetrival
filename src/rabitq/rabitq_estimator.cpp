#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/popcount.h"

#include <cmath>
#include <cstring>

namespace vdb {
namespace rabitq {

// ============================================================================
// Construction
// ============================================================================

RaBitQEstimator::RaBitQEstimator(Dim dim)
    : dim_(dim),
      num_words_((dim + 63) / 64),
      inv_sqrt_dim_(1.0f / std::sqrt(static_cast<float>(dim))) {}

// ============================================================================
// PrepareQuery
// ============================================================================

PreparedQuery RaBitQEstimator::PrepareQuery(
    const float* query,
    const float* centroid,
    const RotationMatrix& rotation) const {
    const size_t L = dim_;

    PreparedQuery pq;
    pq.dim       = dim_;
    pq.num_words = num_words_;

    // Step 1: Center — r = q - c
    std::vector<float> residual(L);
    if (centroid != nullptr) {
        for (size_t i = 0; i < L; ++i) {
            residual[i] = query[i] - centroid[i];
        }
    } else {
        std::memcpy(residual.data(), query, L * sizeof(float));
    }

    // Step 2: Compute ‖q - c‖₂
    float norm_sq = 0.0f;
    for (size_t i = 0; i < L; ++i) {
        norm_sq += residual[i] * residual[i];
    }
    pq.norm_qc_sq = norm_sq;
    pq.norm_qc    = std::sqrt(norm_sq);

    // Step 3: Normalize — q̄ = r / ‖r‖
    if (pq.norm_qc > 1e-30f) {
        float inv_norm = 1.0f / pq.norm_qc;
        for (size_t i = 0; i < L; ++i) {
            residual[i] *= inv_norm;
        }
    }

    // Step 4: Rotate — q' = P^T × q̄
    pq.rotated.resize(L);
    rotation.Apply(residual.data(), pq.rotated.data());

    // Step 5: Sign-quantize q' → packed sign code
    pq.sign_code.resize(num_words_, 0ULL);
    for (size_t i = 0; i < L; ++i) {
        if (pq.rotated[i] >= 0.0f) {
            size_t word_idx = i / 64;
            size_t bit_idx  = i % 64;
            pq.sign_code[word_idx] |= (1ULL << bit_idx);
        }
    }

    // Step 6: Precompute sum_q = Σ q'[i]
    pq.sum_q = 0.0f;
    for (size_t i = 0; i < L; ++i) {
        pq.sum_q += pq.rotated[i];
    }

    return pq;
}

// ============================================================================
// EstimateDistance — fast XOR+popcount path
// ============================================================================

float RaBitQEstimator::EstimateDistance(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    return EstimateDistanceRaw(pq, code.code.data(), num_words_, code.norm);
}

// ============================================================================
// EstimateDistanceRaw — zero-copy fast XOR+popcount path
// ============================================================================

float RaBitQEstimator::EstimateDistanceRaw(
    const PreparedQuery& pq,
    const uint64_t* code_words,
    uint32_t num_words,
    float norm_oc) const {
    // Hamming distance = popcount(q_sign XOR db_code)
    uint32_t hamming = simd::PopcountXor(
        pq.sign_code.data(), code_words, num_words);

    // Popcount-based inner product estimate:
    //   ⟨q̄, ô⟩ ≈ 1 - 2·hamming/L
    //
    // Derivation: approximate q'[i] ≈ sign(q'[i]) / √L, then
    //   ⟨q̄, ô⟩ ≈ (1/L) × Σ sign(q'[i]) × (2·x[i] - 1)
    //           = (1/L) × (L - 2·hamming) = 1 - 2·hamming/L
    float ip_est = 1.0f - 2.0f * static_cast<float>(hamming) /
                                  static_cast<float>(dim_);

    // Full distance: ‖o-q‖² ≈ ‖o-c‖² + ‖q-c‖² - 2·‖o-c‖·‖q-c‖·⟨q̄,ô⟩
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return std::max(dist_sq, 0.0f);
}

// ============================================================================
// EstimateDistanceAccurate — float dot product path
// ============================================================================

float RaBitQEstimator::EstimateDistanceAccurate(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    // Compute ⟨q̄, ô⟩ directly:
    //   ô[i] = (2·x[i] - 1) / √L
    //   ⟨q̄, ô⟩ = (1/√L) Σ q'[i] × (2·x[i] - 1)

    float dot = 0.0f;
    for (size_t i = 0; i < dim_; ++i) {
        size_t word_idx = i / 64;
        size_t bit_idx  = i % 64;
        int bit = (code.code[word_idx] >> bit_idx) & 1;
        float sign = static_cast<float>(2 * bit - 1);  // +1 or -1
        dot += pq.rotated[i] * sign;
    }

    float ip_est = dot * inv_sqrt_dim_;

    // Full distance formula
    float norm_oc = code.norm;
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return dist_sq;
}

// ============================================================================
// EstimateDistanceBatch
// ============================================================================

void RaBitQEstimator::EstimateDistanceBatch(
    const PreparedQuery& pq,
    const RaBitQCode* codes,
    uint32_t n,
    float* out_dist) const {
    for (uint32_t i = 0; i < n; ++i) {
        out_dist[i] = EstimateDistance(pq, codes[i]);
    }
}

}  // namespace rabitq
}  // namespace vdb
