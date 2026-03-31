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
/// @param sign       Per-dimension sign flags (uint8_t: 1=positive, 0=negative, length = dim)
/// @param dim        Vector dimensionality
/// @return           Raw inner product (to be multiplied by xipnorm)
float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 Dim dim);

}  // namespace simd
}  // namespace vdb
