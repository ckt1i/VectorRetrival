#pragma once

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

#include <cstdint>

namespace vdb {
namespace simd {

/// Compute the signed inner product for ExRaBitQ Stage 2:
///
///   result = Σ query[i] * sign[i] * (code_abs[i] + 0.5)
///
/// @param query      Rotated query vector (float, length = dim)
/// @param code_abs   Per-dimension quantized absolute values (uint8_t, length = dim)
/// @param sign       Sign payload, either packed bits or per-dimension flags
/// @param sign_packed Whether `sign` uses packed-bit layout
/// @param dim        Vector dimensionality
/// @return           Raw inner product (to be multiplied by xipnorm)
float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 bool sign_packed,
                 Dim dim);

/// Packed-sign specialized Stage2 kernel for v10 serving path.
float IPExRaBitQPackedSign(const float* VDB_RESTRICT query,
                           const uint8_t* VDB_RESTRICT code_abs,
                           const uint8_t* VDB_RESTRICT packed_sign,
                           Dim dim);

/// Batch packed-sign Stage2 kernel.
/// `code_abs_ptrs`, `packed_sign_ptrs`, and `out_ip_raw` must all have length >= count.
void IPExRaBitQBatchPackedSign(const float* VDB_RESTRICT query,
                               const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
                               const uint8_t* const* VDB_RESTRICT packed_sign_ptrs,
                               uint32_t count,
                               Dim dim,
                               float* VDB_RESTRICT out_ip_raw);

}  // namespace simd
}  // namespace vdb
