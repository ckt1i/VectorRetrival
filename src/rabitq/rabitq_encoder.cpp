#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/simd/popcount.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <vector>

namespace vdb {
namespace rabitq {

// ============================================================================
// Construction
// ============================================================================

RaBitQEncoder::RaBitQEncoder(Dim dim, const RotationMatrix& rotation,
                             uint8_t bits)
    : dim_(dim),
      bits_(bits),
      words_per_plane_((dim + 63) / 64),
      total_words_(static_cast<uint32_t>(bits) * ((dim + 63) / 64)),
      rotation_(rotation) {}

// ============================================================================
// Encode — single vector
// ============================================================================

RaBitQCode RaBitQEncoder::Encode(const float* vec,
                                  const float* centroid) const {
    const size_t L = dim_;
    const uint8_t M = bits_;
    const uint32_t wpp = words_per_plane_;

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

    // Step 5 & 6: M-bit quantize and pack into bit-plane layout
    RaBitQCode result;
    result.code.resize(total_words_, 0ULL);
    result.norm = norm;
    result.bits = M;

    const uint32_t num_levels = 1u << M;  // 2^M
    const int max_code = static_cast<int>(num_levels - 1);

    if (M == 1) {
        // 1-bit: sign quantization (original path, unchanged)
        for (size_t i = 0; i < L; ++i) {
            if (rotated[i] >= 0.0f) {
                size_t word_idx = i / 64;
                size_t bit_idx  = i % 64;
                result.code[word_idx] |= (1ULL << bit_idx);
            }
        }
    } else {
        // M-bit: sign-based bit-plane (for Stage 1) + fast_quantize ex_code (for Stage 2)

        // --- Bit-plane: same as original uniform quantization for Stage 1 ---
        const float scale = static_cast<float>(num_levels) * 0.5f;
        for (size_t i = 0; i < L; ++i) {
            float v = rotated[i];
            int bin = static_cast<int>(std::floor((v + 1.0f) * scale));
            bin = std::clamp(bin, 0, max_code);

            size_t word_idx = i / 64;
            size_t bit_idx  = i % 64;
            for (uint8_t p = 0; p < M; ++p) {
                int bit = (bin >> (M - 1 - p)) & 1;
                if (bit) {
                    result.code[static_cast<size_t>(p) * wpp + word_idx] |=
                        (1ULL << bit_idx);
                }
            }
        }

        // --- ExRaBitQ: fast_quantize for Stage 2 ---
        constexpr double eps = 1e-5;
        constexpr int n_enum = 10;

        std::vector<float> abs_rot(L);
        for (size_t i = 0; i < L; ++i) {
            abs_rot[i] = std::abs(rotated[i]);
        }

        double max_o = *std::max_element(abs_rot.begin(), abs_rot.end());
        if (max_o < eps) max_o = eps;

        double t_start = static_cast<double>(max_code / 3) / max_o;
        double t_end = static_cast<double>(max_code + n_enum) / max_o;

        std::vector<int> cur_code(L);
        double sqr_denom = L * 0.25;
        double numer = 0.0;

        for (size_t i = 0; i < L; ++i) {
            cur_code[i] = static_cast<int>(t_start * abs_rot[i] + eps);
            if (cur_code[i] > max_code) cur_code[i] = max_code;
            sqr_denom += static_cast<double>(cur_code[i]) * cur_code[i]
                       + cur_code[i];
            numer += (cur_code[i] + 0.5) * abs_rot[i];
        }

        using PQEntry = std::pair<double, size_t>;
        std::priority_queue<PQEntry, std::vector<PQEntry>,
                            std::greater<PQEntry>> pq;
        for (size_t i = 0; i < L; ++i) {
            if (abs_rot[i] > eps && cur_code[i] < max_code) {
                double next_t = static_cast<double>(cur_code[i] + 1)
                              / abs_rot[i];
                if (next_t < t_end) {
                    pq.emplace(next_t, i);
                }
            }
        }

        double max_ip = 0.0;
        double best_t = t_start;

        while (!pq.empty()) {
            auto [cur_t, idx] = pq.top();
            pq.pop();

            cur_code[idx]++;
            int updated = cur_code[idx];
            sqr_denom += 2.0 * updated;
            numer += abs_rot[idx];

            double cur_ip = numer / std::sqrt(sqr_denom);
            if (cur_ip > max_ip) {
                max_ip = cur_ip;
                best_t = cur_t;
            }

            if (updated < max_code) {
                double next_t = static_cast<double>(updated + 1)
                              / abs_rot[idx];
                if (next_t < t_end) {
                    pq.emplace(next_t, idx);
                }
            }
        }

        // Re-quantize with optimal t
        result.ex_code.resize(L);
        result.ex_sign.resize(L);
        double final_numer = 0.0;
        for (size_t i = 0; i < L; ++i) {
            int ca = static_cast<int>(best_t * abs_rot[i] + eps);
            if (ca > max_code) ca = max_code;
            result.ex_code[i] = static_cast<uint8_t>(ca);
            result.ex_sign[i] = (rotated[i] >= 0.0f);
            final_numer += (ca + 0.5) * abs_rot[i];
        }
        result.xipnorm = (final_numer > 1e-30)
                        ? static_cast<float>(1.0 / final_numer) : 1.0f;
    }

    // Precompute sum_x = popcount(MSB plane only)
    result.sum_x = simd::PopcountTotal(result.code.data(), wpp);

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
