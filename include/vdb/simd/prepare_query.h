#pragma once

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

#include <cstdint>

namespace vdb {
namespace simd {

// prepare_query.cpp owns the query-global arithmetic up to normalized rotated
// query / sign packing / max-abs extraction. FastScan-specific quantize+LUT
// generation is intentionally kept in fastscan.cpp so reference and fused
// paths can share one contract and one equivalence boundary.

// ============================================================================
// SimdSubtractAndNormSq
//   Computes out[i] = a[i] - b[i] for all i, and returns Σ out[i]².
//   If b == nullptr, copies a into out and returns Σ a[i]².
//   Single memory pass: subtract and accumulate squared differences together.
// ============================================================================

float SimdSubtractAndNormSq(const float* VDB_RESTRICT a,
                            const float* VDB_RESTRICT b,
                            float* VDB_RESTRICT out,
                            uint32_t dim);

struct NormalizeSignSumResult {
    float sum = 0.0f;
    float max_abs = 0.0f;
};

// ============================================================================
// SimdNormalizeSignSum
//   In a single pass over vec[0..dim):
//   - Multiplies each element in-place by inv_norm (normalize)
//   - Packs sign bits into sign_code_words (1 = element >= 0)
//   - Accumulates the sum of all (scaled) elements
//   Returns Σ (vec[i] * inv_norm).
//   sign_code_words must be zeroed by the caller before calling.
// ============================================================================

float SimdNormalizeSignSum(float* VDB_RESTRICT vec,
                           float inv_norm,
                           uint64_t* sign_code_words,
                           uint32_t num_words,
                           uint32_t dim);

NormalizeSignSumResult SimdNormalizeSignSumMaxAbs(
    float* VDB_RESTRICT vec,
    float inv_norm,
    uint64_t* sign_code_words,
    uint32_t num_words,
    uint32_t dim);

}  // namespace simd
}  // namespace vdb
