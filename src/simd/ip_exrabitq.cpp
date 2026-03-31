#include "vdb/simd/ip_exrabitq.h"

#ifdef VDB_USE_AVX2
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

// ============================================================================
// AVX-512 implementation
// ============================================================================
#if defined(VDB_USE_AVX512)

float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 Dim dim) {
    __m512 sum = _mm512_setzero_ps();
    const __m512 half = _mm512_set1_ps(0.5f);
    const __m512 neg_one = _mm512_set1_ps(-1.0f);
    const __m512 pos_one = _mm512_set1_ps(1.0f);
    const __m512i zero = _mm512_setzero_si512();
    uint32_t i = 0;

    for (; i + 16 <= dim; i += 16) {
        __m512 q = _mm512_loadu_ps(query + i);

        __m128i codes_16 = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(code_abs + i));
        __m512i codes_32 = _mm512_cvtepu8_epi32(codes_16);
        __m512 codes_f = _mm512_cvtepi32_ps(codes_32);
        __m512 recon = _mm512_add_ps(codes_f, half);

        __m128i sign_16 = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(sign + i));
        __m512i sign_32 = _mm512_cvtepu8_epi32(sign_16);
        __mmask16 sign_mask = _mm512_cmpneq_epi32_mask(sign_32, zero);
        __m512 sign_f = _mm512_mask_blend_ps(sign_mask, neg_one, pos_one);

        __m512 val = _mm512_mul_ps(recon, sign_f);
        sum = _mm512_fmadd_ps(q, val, sum);
    }

    float result = _mm512_reduce_add_ps(sum);

    for (; i < dim; i++) {
        float s = sign[i] ? 1.0f : -1.0f;
        result += query[i] * s * (static_cast<float>(code_abs[i]) + 0.5f);
    }

    return result;
}

// ============================================================================
// AVX2 implementation
// ============================================================================
#elif defined(VDB_USE_AVX2)

namespace {

VDB_FORCE_INLINE float HorizAdd(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}

}  // namespace

float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 Dim dim) {
    __m256 sum = _mm256_setzero_ps();
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 neg_one = _mm256_set1_ps(-1.0f);
    const __m256 pos_one = _mm256_set1_ps(1.0f);
    uint32_t i = 0;

    for (; i + 8 <= dim; i += 8) {
        // Load 8 query floats
        __m256 q = _mm256_loadu_ps(query + i);

        // Load 8 code_abs bytes → convert to float + 0.5
        __m128i codes_8 = _mm_loadl_epi64(
            reinterpret_cast<const __m128i*>(code_abs + i));
        __m256i codes_32 = _mm256_cvtepu8_epi32(codes_8);
        __m256 codes_f = _mm256_cvtepi32_ps(codes_32);
        __m256 recon = _mm256_add_ps(codes_f, half);  // code_abs + 0.5

        // Load 8 sign bytes → convert to float sign multiplier (+1 or -1)
        __m128i sign_8 = _mm_loadl_epi64(
            reinterpret_cast<const __m128i*>(sign + i));
        __m256i sign_32 = _mm256_cvtepu8_epi32(sign_8);
        // sign_mask: 0 → -1.0, 1 → +1.0
        __m256 sign_f = _mm256_blendv_ps(
            neg_one, pos_one,
            _mm256_castsi256_ps(_mm256_slli_epi32(sign_32, 31)));

        // ip += query * sign * (code_abs + 0.5)
        __m256 val = _mm256_mul_ps(recon, sign_f);
        sum = _mm256_fmadd_ps(q, val, sum);
    }

    float result = HorizAdd(sum);

    // Scalar tail
    for (; i < dim; i++) {
        float s = sign[i] ? 1.0f : -1.0f;
        result += query[i] * s * (static_cast<float>(code_abs[i]) + 0.5f);
    }

    return result;
}

// ============================================================================
// Scalar fallback
// ============================================================================
#else

float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 Dim dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        float s = sign[i] ? 1.0f : -1.0f;
        sum += query[i] * s * (static_cast<float>(code_abs[i]) + 0.5f);
    }
    return sum;
}

#endif  // VDB_USE_AVX2

}  // namespace simd
}  // namespace vdb
