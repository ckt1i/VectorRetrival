#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"
#include "vdb/rabitq/rabitq_rotation.h"

namespace vdb {
namespace rabitq {

// ============================================================================
// RaBitQ Encoded Vector — output of the encoder
// ============================================================================

/// A single vector encoded by RaBitQ 1-bit quantization.
///
/// Contains:
///   - `code`:  Binary code packed as uint64_t words. code[i] bit j represents
///              dimension (i*64 + j). A set bit means the rotated normalized
///              component was >= 0; cleared means < 0.
///   - `norm`:  ‖o - c‖₂  (the L2 norm of the residual before normalization).
///              Needed in the distance formula to reconstruct the scale.
///   - `sum_x`: Total number of set bits in `code` (= popcount of entire code).
///              Precomputed for the distance formula.
///
struct RaBitQCode {
    std::vector<uint64_t> code;   // Packed binary code, ceil(dim/64) words
    float norm;                    // ‖o - c‖₂
    uint32_t sum_x;                // popcount(code)
};

// ============================================================================
// RaBitQEncoder — encodes float vectors into 1-bit RaBitQ codes
// ============================================================================

/// Encoder for RaBitQ 1-bit quantization.
///
/// The encoding pipeline for each database vector o:
///
///   1. **Center:**     r = o - c           (subtract cluster centroid)
///   2. **Norm:**       ‖r‖ = ‖o - c‖₂     (store this for distance formula)
///   3. **Normalize:**  ō = r / ‖r‖        (unit vector)
///   4. **Rotate:**     ō' = P^T × ō       (rotate into canonical basis)
///   5. **Sign-quantize:** x[i] = (ō'[i] >= 0) ? 1 : 0
///   6. **Pack:**       pack x[] into uint64_t[ceil(dim/64)]
///
/// The rotation matrix P is shared across all vectors in the index.
/// The centroid c is per-cluster (pass nullptr for flat index / testing).
///
class RaBitQEncoder {
 public:
    /// Construct an encoder for vectors of dimension `dim` using rotation `P`.
    ///
    /// @param dim  Vector dimensionality
    /// @param rotation  Shared rotation matrix (must outlive the encoder).
    ///                  The encoder does NOT take ownership.
    RaBitQEncoder(Dim dim, const RotationMatrix& rotation);

    ~RaBitQEncoder() = default;
    VDB_DISALLOW_COPY_AND_MOVE(RaBitQEncoder);

    /// Encode a single vector.
    ///
    /// @param vec       Raw float vector (length = dim)
    /// @param centroid  Cluster centroid (length = dim). Pass nullptr to use
    ///                  the zero vector (flat index / testing).
    /// @return          Encoded RaBitQ code with norm and popcount.
    RaBitQCode Encode(const float* vec, const float* centroid = nullptr) const;

    /// Encode a batch of vectors.
    ///
    /// @param vecs      Row-major array of n vectors (n × dim floats)
    /// @param n         Number of vectors to encode
    /// @param centroid  Shared cluster centroid (or nullptr for zero)
    /// @return          Vector of encoded codes (size = n)
    std::vector<RaBitQCode> EncodeBatch(const float* vecs, uint32_t n,
                                         const float* centroid = nullptr) const;

    /// Number of uint64_t words per code.
    uint32_t num_code_words() const { return num_words_; }

    /// Vector dimensionality.
    Dim dim() const { return dim_; }

 private:
    Dim dim_;
    uint32_t num_words_;              // ceil(dim / 64)
    const RotationMatrix& rotation_;
};

}  // namespace rabitq
}  // namespace vdb
