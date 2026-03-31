#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/fastscan.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/simd/popcount.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vdb {
namespace rabitq {

// ============================================================================
// Construction
// ============================================================================

RaBitQEstimator::RaBitQEstimator(Dim dim, uint8_t bits)
    : dim_(dim),
      bits_(bits),
      words_per_plane_((dim + 63) / 64),
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
    pq.num_words = words_per_plane_;
    pq.bits      = bits_;

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
    pq.sign_code.resize(words_per_plane_, 0ULL);
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

    // Step 7: FastScan query quantization + LUT construction
    pq.quant_query.resize(L);
    pq.fs_width = simd::QuantizeQuery14Bit(
        pq.rotated.data(), pq.quant_query.data(), dim_);

    // Allocate 64-byte aligned LUT buffer.
    // Two-plane LUT: lo + hi byte planes, ceil(M/4)*128 bytes = dim*8 bytes.
    const size_t lut_size = static_cast<size_t>(dim_) * 8;
    pq.fastscan_lut.resize(lut_size + 63);
    // Compute 64-byte aligned pointer
    uintptr_t raw = reinterpret_cast<uintptr_t>(pq.fastscan_lut.data());
    uintptr_t aligned = (raw + 63) & ~uintptr_t(63);
    pq.lut_aligned = reinterpret_cast<uint8_t*>(aligned);
    std::memset(pq.lut_aligned, 0, lut_size);

    pq.fs_shift = simd::BuildFastScanLUT(
        pq.quant_query.data(), pq.lut_aligned, dim_);

    return pq;
}

// ============================================================================
// EstimateDistance — Stage 1: fast XOR+popcount path (MSB plane only)
// ============================================================================

float RaBitQEstimator::EstimateDistance(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    // Use only MSB plane (first words_per_plane_ words)
    return EstimateDistanceRaw(pq, code.code.data(), words_per_plane_,
                               code.norm);
}

// ============================================================================
// EstimateDistanceRaw — Stage 1: zero-copy fast XOR+popcount path
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
    float ip_est = 1.0f - 2.0f * static_cast<float>(hamming) /
                                  static_cast<float>(dim_);

    // Full distance: ‖o-q‖² ≈ ‖o-c‖² + ‖q-c‖² - 2·‖o-c‖·‖q-c‖·⟨q̄,ô⟩
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return std::max(dist_sq, 0.0f);
}

// ============================================================================
// EstimateDistanceFastScan — batch-32 VPSHUFB path
// ============================================================================

void RaBitQEstimator::EstimateDistanceFastScan(
    const PreparedQuery& pq,
    const uint8_t* packed_codes,
    const float* block_norms,
    uint32_t count,
    float* out_dist) const {
    // 1. VPSHUFB accumulation: get raw LUT sums for 32 vectors
    alignas(64) uint32_t raw_accu[32] = {0};
    simd::AccumulateBlock(packed_codes, pq.lut_aligned, raw_accu, dim_);

    // 2. De-quantize and compute distances.
    //
    // raw_accu[v] = Σ_{d where x[v][d]=1} (quant_q[d] - v_min_per_group)
    //
    // After shift + width:
    //   ip_raw = (raw_accu[v] + fs_shift) * fs_width  ≈  Σ_{x[d]=1} q'[d]
    //
    // Inner product estimate (matching EstimateDistanceAccurate):
    //   ⟨q̄, ô⟩ = (1/√D) * (2*ip_raw - sum_q)
    //
    // Distance:
    //   dist = norm_oc² + norm_qc² - 2*norm_oc*norm_qc*⟨q̄,ô⟩

    for (uint32_t v = 0; v < count; ++v) {
        float ip_raw = (static_cast<float>(raw_accu[v]) +
                        static_cast<float>(pq.fs_shift)) * pq.fs_width;
        float ip_est = (2.0f * ip_raw - pq.sum_q) * inv_sqrt_dim_;
        float dist_sq = block_norms[v] * block_norms[v] + pq.norm_qc_sq
                        - 2.0f * block_norms[v] * pq.norm_qc * ip_est;
        out_dist[v] = std::max(dist_sq, 0.0f);
    }
}

// ============================================================================
// EstimateDistanceAccurate — float dot product path (1-bit)
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
// EstimateDistanceMultiBit — Stage 2: M-bit LUT scan
// ============================================================================

float RaBitQEstimator::EstimateDistanceMultiBit(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    // Use ex_code/ex_sign (fast_quantize) when available — SIMD accelerated
    if (!code.ex_code.empty()) {
        float ip_raw = simd::IPExRaBitQ(
            pq.rotated.data(), code.ex_code.data(),
            code.ex_sign.data(), dim_);
        float ip_est = ip_raw * code.xipnorm;
        float dist_sq = code.norm * code.norm + pq.norm_qc_sq
                      - 2.0f * code.norm * pq.norm_qc * ip_est;
        return std::max(dist_sq, 0.0f);
    }
    // Fallback to bit-plane extraction
    return EstimateDistanceMultiBitRaw(pq, code.code.data(),
                                       words_per_plane_, code.bits,
                                       code.norm, code.xipnorm);
}

// ============================================================================
// EstimateDistanceMultiBitRaw — Stage 2: xipnorm-corrected M-bit estimate
// ============================================================================

float RaBitQEstimator::EstimateDistanceMultiBitRaw(
    const PreparedQuery& pq,
    const uint64_t* code_words,
    uint32_t wpp,
    uint8_t bits,
    float norm_oc,
    float xipnorm) const {
    const uint8_t M = bits;
    const int max_code = (1 << M) - 1;

    // Compute: Σ q'[i] * sign[i] * (code_abs[i] + 0.5)
    // where sign is from MSB plane, code_abs is recovered from sign + code_stored
    //
    // From the bit-plane layout:
    //   MSB plane (plane 0) = sign bit: 1=positive, 0=negative
    //   code_stored = full M-bit value extracted from all planes
    //   code_abs = (sign==1) ? code_stored : (2^M-1) - code_stored
    //
    // Then: sign[i] * (code_abs[i] + 0.5) is the signed reconstruction value
    // and   xipnorm = 1 / Σ (code_abs[i] + 0.5) * |o'[i]|

    float ip_raw = 0.0f;
    for (size_t i = 0; i < dim_; ++i) {
        size_t word_idx = i / 64;
        size_t bit_idx  = i % 64;

        // Extract full M-bit code_stored from all M planes
        int code_stored = 0;
        for (uint8_t p = 0; p < M; ++p) {
            uint32_t bit = (code_words[static_cast<size_t>(p) * wpp +
                                       word_idx] >> bit_idx) & 1;
            code_stored |= (static_cast<int>(bit) << (M - 1 - p));
        }

        // MSB of code_stored ≈ sign (due to sign-flip encoding)
        // sign_bit = MSB → 1=positive, 0=negative
        int sign_bit = (code_stored >> (M - 1)) & 1;
        float sign = sign_bit ? 1.0f : -1.0f;

        // Recover code_abs from code_stored:
        //   positive: code_stored = code_abs → code_abs = code_stored
        //   negative: code_stored = max_code - code_abs → code_abs = max_code - code_stored
        int code_abs = sign_bit ? code_stored
                                : (max_code - code_stored);

        // Signed reconstruction: sign * (code_abs + 0.5)
        float recon = sign * (static_cast<float>(code_abs) + 0.5f);
        ip_raw += pq.rotated[i] * recon;
    }

    // Apply xipnorm correction: ip_est = ip_raw * xipnorm
    float ip_est = ip_raw * xipnorm;

    // Full distance formula
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return std::max(dist_sq, 0.0f);
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
