#pragma once

#include <cstdint>

#include "vdb/common/macros.h"

namespace vdb {
namespace simd {

// ============================================================================
// Fast Walsh-Hadamard Transform (in-place, unnormalized)
// ============================================================================
//
// Both functions perform the in-place FWHT on a length-`n` float vector,
// where `n` MUST be a power of 2. The transform is its own inverse up to
// scaling by 1/n. After this call, vec[] contains the transform coefficients.
//
// Use FWHT_AVX512 when n >= 16 and the build has AVX-512; the early levels
// (len < 16) still go through the scalar inner loop, but levels with
// len >= 16 use _mm512_loadu_ps + add/sub for 16 butterflies per iteration.
//
// FWHTScalar is the reference (and fallback) implementation.

void FWHTScalar(float* VDB_RESTRICT vec, uint32_t n);
void FWHT_AVX512(float* VDB_RESTRICT vec, uint32_t n);

}  // namespace simd
}  // namespace vdb
