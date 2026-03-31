#include "vdb/simd/distance_l2.h"

#ifdef VDB_USE_AVX2
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

// ============================================================================
// AVX-512 implementation
// ============================================================================
#if defined(VDB_USE_AVX512)

float L2Sqr(const float* VDB_RESTRICT a, const float* VDB_RESTRICT b, Dim d) {
    __m512 sum = _mm512_setzero_ps();
    uint32_t i = 0;

    for (; i + 16 <= d; i += 16) {
        __m512 va   = _mm512_loadu_ps(a + i);
        __m512 vb   = _mm512_loadu_ps(b + i);
        __m512 diff = _mm512_sub_ps(va, vb);
        sum = _mm512_fmadd_ps(diff, diff, sum);
    }

    float result = _mm512_reduce_add_ps(sum);

    for (; i < d; i++) {
        float diff = a[i] - b[i];
        result += diff * diff;
    }

    return result;
}

// ============================================================================
// AVX2 implementation
// ============================================================================
#elif defined(VDB_USE_AVX2)

namespace {

/// Horizontal sum of 8 floats packed in a __m256 register.
/// Uses 128-bit hadd to avoid the slow 256-bit horizontal add.
VDB_FORCE_INLINE float HorizAdd(__m256 v) {
    // Fold upper 128-bit lane into lower
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    // Horizontal add within the 4-element __m128
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}

}  // namespace

float L2Sqr(const float* VDB_RESTRICT a, const float* VDB_RESTRICT b, Dim d) {
    __m256 sum = _mm256_setzero_ps();
    uint32_t i = 0;

    // Main loop: 8 floats per iteration using FMA
    for (; i + 8 <= d; i += 8) {
        __m256 va   = _mm256_loadu_ps(a + i);
        __m256 vb   = _mm256_loadu_ps(b + i);
        __m256 diff = _mm256_sub_ps(va, vb);
        // sum += diff * diff  (fused multiply-add)
        sum = _mm256_fmadd_ps(diff, diff, sum);
    }

    float result = HorizAdd(sum);

    // Scalar tail for d % 8 != 0
    for (; i < d; i++) {
        float diff = a[i] - b[i];
        result += diff * diff;
    }

    return result;
}

// ============================================================================
// Scalar fallback (no AVX2)
// ============================================================================
#else

float L2Sqr(const float* VDB_RESTRICT a, const float* VDB_RESTRICT b, Dim d) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < d; i++) {
        float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

#endif  // VDB_USE_AVX2

}  // namespace simd
}  // namespace vdb
