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
///   7. (M>1) Precompute LUT[2^M] for Stage 2 distance estimation
///
struct PreparedQuery {
    std::vector<float>    rotated;     // q' = P^T × q̄  (length = dim)
    std::vector<uint64_t> sign_code;   // Sign bits of q' packed as uint64_t
    float                 norm_qc;     // ‖q - c‖₂
    float                 norm_qc_sq;  // ‖q - c‖₂²
    float                 sum_q;       // Σ q'[i] (sum of all rotated components)
    Dim                   dim;         // Vector dimensionality
    uint32_t              num_words;   // ceil(dim / 64) = words per plane

    // Multi-bit fields
    uint8_t               bits = 1;    // M: quantization bits

    // FastScan fields (populated alongside sign_code in PrepareQuery)
    std::vector<int16_t>  quant_query;   // 14-bit quantized q' (length = dim)
    std::vector<uint8_t>  fastscan_lut;  // Packed VPSHUFB LUT (dim*4 bytes + alignment pad)
    uint8_t*              lut_aligned = nullptr;  // 64-byte aligned pointer into fastscan_lut
    float                 fs_width = 0.0f;  // Quantization step width
    int32_t               fs_shift = 0;     // Accumulated v_min shift from BuildFastScanLUT
};

// ============================================================================
// RaBitQEstimator — approximate distance computation
// ============================================================================

/// Approximate distance estimator for RaBitQ M-bit quantized vectors.
///
/// Supports two estimation paths:
///
/// **Stage 1 (fast, O(L/64)):** XOR+popcount on MSB plane (= 1-bit sign code).
///   Same as original 1-bit RaBitQ. Used for initial SafeIn/SafeOut classification.
///
/// **Stage 2 (accurate, O(L), M-bit LUT scan):** For M>1, uses a precomputed
///   lookup table to compute a more accurate inner product estimate from the
///   full M-bit quantized code. Only applied to Uncertain vectors from Stage 1.
///
class RaBitQEstimator {
 public:
    /// Construct an estimator for the given dimensionality.
    ///
    /// @param dim   Vector dimensionality
    /// @param bits  Quantization bits: 1, 2, or 4 (default 1)
    explicit RaBitQEstimator(Dim dim, uint8_t bits = 1);

    ~RaBitQEstimator() = default;
    VDB_DISALLOW_COPY_AND_MOVE(RaBitQEstimator);

    /// Prepare a query vector for distance estimation.
    ///
    /// When bits > 1, also precomputes the M-bit LUT for Stage 2.
    ///
    /// @param query     Raw query vector (length = dim)
    /// @param centroid  Cluster centroid (length = dim, or nullptr for zero)
    /// @param rotation  Rotation matrix (same as used for encoding)
    /// @return          Prepared query structure
    PreparedQuery PrepareQuery(const float* query,
                               const float* centroid,
                               const RotationMatrix& rotation) const;

    /// Prepare a query into an existing PreparedQuery, reusing allocated buffers.
    ///
    /// Same as PrepareQuery but avoids heap allocations after the first call
    /// by reusing the vectors' existing capacity via resize().
    void PrepareQueryInto(const float* query,
                          const float* centroid,
                          const RotationMatrix& rotation,
                          PreparedQuery* pq) const;

    /// Stage 1: Estimate distance using fast XOR+popcount on MSB plane.
    ///
    /// Uses only the first words_per_plane words of the code (MSB plane).
    /// Result is identical regardless of M — always uses 1-bit approximation.
    float EstimateDistance(const PreparedQuery& pq,
                           const RaBitQCode& code) const;

    /// Estimate distance using the more accurate float-dot path (1-bit).
    float EstimateDistanceAccurate(const PreparedQuery& pq,
                                    const RaBitQCode& code) const;

    /// Batch estimate using Stage 1 (fast popcount path).
    void EstimateDistanceBatch(const PreparedQuery& pq,
                                const RaBitQCode* codes,
                                uint32_t n,
                                float* out_dist) const;

    /// Stage 1: Zero-copy distance estimate from raw memory (MSB plane only).
    ///
    /// @param pq          Prepared query
    /// @param code_words  Pointer to MSB plane (words_per_plane uint64_t)
    /// @param num_words   Words per plane (= ceil(dim/64))
    /// @param norm_oc     ‖o - c‖₂
    /// @return            Estimated ‖o - q‖² (clamped to ≥ 0)
    float EstimateDistanceRaw(const PreparedQuery& pq,
                               const uint64_t* code_words,
                               uint32_t num_words,
                               float norm_oc) const;

    /// FastScan Stage 1: batch-32 distance estimation using VPSHUFB.
    ///
    /// Processes one FastScan block of up to 32 vectors simultaneously.
    /// Uses 14-bit quantized query for higher precision than PopcountXor.
    ///
    /// @param pq           Prepared query (must have fastscan_lut populated)
    /// @param packed_codes  Packed sign bits in VPSHUFB block-32 layout (dim*4 bytes)
    /// @param block_norms   Per-vector norm_oc[32] (from FastScan block factors)
    /// @param count         Actual vectors in this block (1..32)
    /// @param out_dist      Output: estimated distances (at least 32 floats)
    void EstimateDistanceFastScan(const PreparedQuery& pq,
                                   const uint8_t* packed_codes,
                                   const float* block_norms,
                                   uint32_t count,
                                   float* out_dist) const;

    /// Stage 2: Estimate distance using full M-bit LUT scan.
    ///
    /// Extracts the M-bit quantized value for each dimension from the
    /// bit-plane layout, looks up the reconstruction value from the LUT,
    /// and computes a more accurate inner product estimate.
    ///
    /// @param pq    Prepared query (must have lut populated, i.e. bits > 1)
    /// @param code  Database vector's RaBitQ code (M-bit encoded)
    /// @return      Estimated ‖o - q‖² (clamped to ≥ 0)
    float EstimateDistanceMultiBit(const PreparedQuery& pq,
                                    const RaBitQCode& code) const;

    /// Stage 2: Zero-copy M-bit distance estimate from raw memory.
    ///
    /// Uses xipnorm correction factor for accurate inner product estimation.
    ///
    /// @param pq              Prepared query
    /// @param code_words      Pointer to full M-plane code (M * wpp uint64_t)
    /// @param words_per_plane Words per plane (= ceil(dim/64))
    /// @param bits            M: quantization bits
    /// @param norm_oc         ‖o - c‖₂
    /// @param xipnorm         Correction factor: 1 / ⟨ō_q, ō'⟩
    /// @return                Estimated ‖o - q‖² (clamped to ≥ 0)
    float EstimateDistanceMultiBitRaw(const PreparedQuery& pq,
                                       const uint64_t* code_words,
                                       uint32_t words_per_plane,
                                       uint8_t bits,
                                       float norm_oc,
                                       float xipnorm) const;

    Dim dim() const { return dim_; }
    uint8_t bits() const { return bits_; }
    uint32_t words_per_plane() const { return words_per_plane_; }

 private:
    Dim dim_;
    uint8_t bits_;                // M: quantization bits
    uint32_t words_per_plane_;    // ceil(dim / 64)
    float inv_sqrt_dim_;          // 1 / √dim  (precomputed)
};

}  // namespace rabitq
}  // namespace vdb
