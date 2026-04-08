#include "vdb/simd/hadamard.h"

#if defined(VDB_USE_AVX512)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

// ============================================================================
// Scalar (reference) implementation
// ============================================================================

void FWHTScalar(float* VDB_RESTRICT vec, uint32_t n) {
    for (uint32_t len = 1; len < n; len <<= 1) {
        for (uint32_t i = 0; i < n; i += len << 1) {
            for (uint32_t j = 0; j < len; ++j) {
                float u = vec[i + j];
                float v = vec[i + j + len];
                vec[i + j]       = u + v;
                vec[i + j + len] = u - v;
            }
        }
    }
}

// ============================================================================
// AVX-512 implementation
// ============================================================================
//
// Strategy:
//   For each level `len in {1, 2, 4, 8, 16, 32, ...}`:
//     - len < 16: use scalar inner loop (relative cost is low; 4 levels × 256
//       butterflies = 1024 ops on n=512, vs ~5000 total).
//     - len >= 16: process 16 butterflies per iteration with AVX-512 (one
//       _mm512_loadu_ps for u, one for v, then add/sub/store).
//
// All levels operate in-place on the same buffer.

void FWHT_AVX512(float* VDB_RESTRICT vec, uint32_t n) {
#if defined(VDB_USE_AVX512)
    // Levels with len < 16: scalar
    uint32_t len = 1;
    for (; len < 16 && len < n; len <<= 1) {
        for (uint32_t i = 0; i < n; i += len << 1) {
            for (uint32_t j = 0; j < len; ++j) {
                float u = vec[i + j];
                float v = vec[i + j + len];
                vec[i + j]       = u + v;
                vec[i + j + len] = u - v;
            }
        }
    }

    // Levels with len >= 16: AVX-512 (16 butterflies per iteration)
    for (; len < n; len <<= 1) {
        for (uint32_t i = 0; i < n; i += len << 1) {
            for (uint32_t j = 0; j < len; j += 16) {
                __m512 u = _mm512_loadu_ps(vec + i + j);
                __m512 v = _mm512_loadu_ps(vec + i + j + len);
                _mm512_storeu_ps(vec + i + j,       _mm512_add_ps(u, v));
                _mm512_storeu_ps(vec + i + j + len, _mm512_sub_ps(u, v));
            }
        }
    }
#else
    // Fallback to scalar when AVX-512 is not available
    FWHTScalar(vec, n);
#endif
}

}  // namespace simd
}  // namespace vdb
