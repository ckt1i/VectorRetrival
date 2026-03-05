#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/simd/popcount.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace vdb {
namespace rabitq {

// ============================================================================
// Construction
// ============================================================================

RaBitQEncoder::RaBitQEncoder(Dim dim, const RotationMatrix& rotation)
    : dim_(dim),
      num_words_((dim + 63) / 64),
      rotation_(rotation) {}

// ============================================================================
// Encode — single vector
// ============================================================================

RaBitQCode RaBitQEncoder::Encode(const float* vec,
                                  const float* centroid) const {
    const size_t L = dim_;

    // Temporary buffers for the pipeline
    std::vector<float> residual(L);
    std::vector<float> rotated(L);

    // Step 1: Center — r = o - c
    if (centroid != nullptr) {
        for (size_t i = 0; i < L; ++i) {
            residual[i] = vec[i] - centroid[i];
        }
    } else {
        std::memcpy(residual.data(), vec, L * sizeof(float));
    }

    // Step 2: Compute norm ‖r‖₂
    float norm_sq = 0.0f;
    for (size_t i = 0; i < L; ++i) {
        norm_sq += residual[i] * residual[i];
    }
    float norm = std::sqrt(norm_sq);

    // Step 3: Normalize — ō = r / ‖r‖
    // Guard against zero-norm vectors (map to zero vector → all bits 0 after sign)
    if (norm > 1e-30f) {
        float inv_norm = 1.0f / norm;
        for (size_t i = 0; i < L; ++i) {
            residual[i] *= inv_norm;
        }
    }

    // Step 4: Rotate — ō' = P^T × ō
    rotation_.Apply(residual.data(), rotated.data());

    // Step 5 & 6: Sign-quantize and pack into uint64_t words
    RaBitQCode result;
    result.code.resize(num_words_, 0ULL);
    result.norm = norm;

    for (size_t i = 0; i < L; ++i) {
        if (rotated[i] >= 0.0f) {
            size_t word_idx = i / 64;
            size_t bit_idx  = i % 64;
            result.code[word_idx] |= (1ULL << bit_idx);
        }
    }

    // Precompute sum_x = popcount(code)
    result.sum_x = simd::PopcountTotal(result.code.data(), num_words_);

    return result;
}

// ============================================================================
// EncodeBatch — multiple vectors
// ============================================================================

std::vector<RaBitQCode> RaBitQEncoder::EncodeBatch(
    const float* vecs, uint32_t n, const float* centroid) const {
    std::vector<RaBitQCode> results;
    results.reserve(n);

    for (uint32_t i = 0; i < n; ++i) {
        results.push_back(Encode(vecs + static_cast<size_t>(i) * dim_, centroid));
    }

    return results;
}

}  // namespace rabitq
}  // namespace vdb
