#include "vdb/simd/ip_exrabitq.h"

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

VDB_FORCE_INLINE float IPExRaBitQReference(const float* VDB_RESTRICT query,
                                          const uint8_t* VDB_RESTRICT code_abs,
                                          const uint8_t* VDB_RESTRICT sign,
                                          Dim dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        const float s = sign[i] ? 1.0f : -1.0f;
        sum += query[i] * s * (static_cast<float>(code_abs[i]) + 0.5f);
    }
    return sum;
}

#if defined(VDB_USE_AVX512)
VDB_FORCE_INLINE __m512 LoadAbsMagnitude16Avx512(
    const uint8_t* VDB_RESTRICT code_abs) {
    const __m128i codes_16 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(code_abs));
    const __m512i codes_32 = _mm512_cvtepu8_epi32(codes_16);
    return _mm512_cvtepi32_ps(codes_32);
}

VDB_FORCE_INLINE __m512 LoadSignedQuery16Avx512(
    const float* VDB_RESTRICT query,
    const uint8_t* VDB_RESTRICT sign) {
    const __m512i sign_bit = _mm512_set1_epi32(static_cast<int>(0x80000000u));
    const __m128i zero = _mm_setzero_si128();
    const __m512 q = _mm512_loadu_ps(query);

    const __m128i sign_16 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(sign));
    const __mmask16 neg_mask = _mm_cmpeq_epi8_mask(sign_16, zero);
    const __m512i sign_flip = _mm512_maskz_mov_epi32(neg_mask, sign_bit);

    return _mm512_castsi512_ps(
        _mm512_xor_si512(_mm512_castps_si512(q), sign_flip));
}
#endif

#if defined(VDB_USE_AVX2)
VDB_FORCE_INLINE __m256 LoadAbsMagnitude8Avx2(
    const uint8_t* VDB_RESTRICT code_abs) {
    const __m128i codes_8 = _mm_loadl_epi64(
        reinterpret_cast<const __m128i*>(code_abs));
    const __m256i codes_32 = _mm256_cvtepu8_epi32(codes_8);
    return _mm256_cvtepi32_ps(codes_32);
}

VDB_FORCE_INLINE __m256 LoadSignedQuery8Avx2(
    const float* VDB_RESTRICT query,
    const uint8_t* VDB_RESTRICT sign) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i sign_bit = _mm256_set1_epi32(static_cast<int>(0x80000000u));
    const __m256 q = _mm256_loadu_ps(query);

    const __m128i sign_8 = _mm_loadl_epi64(
        reinterpret_cast<const __m128i*>(sign));
    const __m256i sign_32 = _mm256_cvtepu8_epi32(sign_8);
    const __m256i neg_mask = _mm256_cmpeq_epi32(sign_32, zero);
    const __m256i sign_flip = _mm256_and_si256(neg_mask, sign_bit);

    return _mm256_xor_ps(q, _mm256_castsi256_ps(sign_flip));
}

VDB_FORCE_INLINE float HorizAdd(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}
#endif

}  // namespace

// ============================================================================
// AVX-512 implementation
// ============================================================================
#if defined(VDB_USE_AVX512)

float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 Dim dim) {
    __m512 dot0 = _mm512_setzero_ps();
    __m512 dot1 = _mm512_setzero_ps();
    __m512 bias0 = _mm512_setzero_ps();
    __m512 bias1 = _mm512_setzero_ps();
    const __m512 half = _mm512_set1_ps(0.5f);
    uint32_t i = 0;

    for (; i + 64 <= dim; i += 64) {
        const __m512 q0 = LoadSignedQuery16Avx512(query + i, sign + i);
        const __m512 q1 = LoadSignedQuery16Avx512(query + i + 16, sign + i + 16);
        const __m512 q2 = LoadSignedQuery16Avx512(query + i + 32, sign + i + 32);
        const __m512 q3 = LoadSignedQuery16Avx512(query + i + 48, sign + i + 48);
        const __m512 a0 = LoadAbsMagnitude16Avx512(code_abs + i);
        const __m512 a1 = LoadAbsMagnitude16Avx512(code_abs + i + 16);
        const __m512 a2 = LoadAbsMagnitude16Avx512(code_abs + i + 32);
        const __m512 a3 = LoadAbsMagnitude16Avx512(code_abs + i + 48);
        dot0 = _mm512_fmadd_ps(q0, a0, dot0);
        dot1 = _mm512_fmadd_ps(q1, a1, dot1);
        dot0 = _mm512_fmadd_ps(q2, a2, dot0);
        dot1 = _mm512_fmadd_ps(q3, a3, dot1);
        bias0 = _mm512_add_ps(bias0, q0);
        bias1 = _mm512_add_ps(bias1, q1);
        bias0 = _mm512_add_ps(bias0, q2);
        bias1 = _mm512_add_ps(bias1, q3);
    }

    for (; i + 32 <= dim; i += 32) {
        const __m512 q0 = LoadSignedQuery16Avx512(query + i, sign + i);
        const __m512 q1 = LoadSignedQuery16Avx512(query + i + 16, sign + i + 16);
        const __m512 a0 = LoadAbsMagnitude16Avx512(code_abs + i);
        const __m512 a1 = LoadAbsMagnitude16Avx512(code_abs + i + 16);
        dot0 = _mm512_fmadd_ps(q0, a0, dot0);
        dot1 = _mm512_fmadd_ps(q1, a1, dot1);
        bias0 = _mm512_add_ps(bias0, q0);
        bias1 = _mm512_add_ps(bias1, q1);
    }

    for (; i + 16 <= dim; i += 16) {
        const __m512 q = LoadSignedQuery16Avx512(query + i, sign + i);
        const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i);
        dot0 = _mm512_fmadd_ps(q, a, dot0);
        bias0 = _mm512_add_ps(bias0, q);
    }

    float result = _mm512_reduce_add_ps(_mm512_add_ps(dot0, dot1));
    result += 0.5f * _mm512_reduce_add_ps(_mm512_add_ps(bias0, bias1));

    for (; i < dim; i++) {
        const float signed_q = sign[i] ? query[i] : -query[i];
        result += signed_q * static_cast<float>(code_abs[i]) + 0.5f * signed_q;
    }
#ifndef NDEBUG
    const float ref = IPExRaBitQReference(query, code_abs, sign, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
    return result;
}

// ============================================================================
// AVX2 implementation
// ============================================================================
#elif defined(VDB_USE_AVX2)
float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 Dim dim) {
    __m256 dot0 = _mm256_setzero_ps();
    __m256 dot1 = _mm256_setzero_ps();
    __m256 bias0 = _mm256_setzero_ps();
    __m256 bias1 = _mm256_setzero_ps();
    uint32_t i = 0;

    for (; i + 32 <= dim; i += 32) {
        const __m256 q0 = LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 q1 = LoadSignedQuery8Avx2(query + i + 8, sign + i + 8);
        const __m256 q2 = LoadSignedQuery8Avx2(query + i + 16, sign + i + 16);
        const __m256 q3 = LoadSignedQuery8Avx2(query + i + 24, sign + i + 24);
        const __m256 a0 = LoadAbsMagnitude8Avx2(code_abs + i);
        const __m256 a1 = LoadAbsMagnitude8Avx2(code_abs + i + 8);
        const __m256 a2 = LoadAbsMagnitude8Avx2(code_abs + i + 16);
        const __m256 a3 = LoadAbsMagnitude8Avx2(code_abs + i + 24);
        dot0 = _mm256_fmadd_ps(q0, a0, dot0);
        dot1 = _mm256_fmadd_ps(q1, a1, dot1);
        dot0 = _mm256_fmadd_ps(q2, a2, dot0);
        dot1 = _mm256_fmadd_ps(q3, a3, dot1);
        bias0 = _mm256_add_ps(bias0, q0);
        bias1 = _mm256_add_ps(bias1, q1);
        bias0 = _mm256_add_ps(bias0, q2);
        bias1 = _mm256_add_ps(bias1, q3);
    }

    for (; i + 16 <= dim; i += 16) {
        const __m256 q0 = LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 q1 = LoadSignedQuery8Avx2(query + i + 8, sign + i + 8);
        const __m256 a0 = LoadAbsMagnitude8Avx2(code_abs + i);
        const __m256 a1 = LoadAbsMagnitude8Avx2(code_abs + i + 8);
        dot0 = _mm256_fmadd_ps(q0, a0, dot0);
        dot1 = _mm256_fmadd_ps(q1, a1, dot1);
        bias0 = _mm256_add_ps(bias0, q0);
        bias1 = _mm256_add_ps(bias1, q1);
    }

    for (; i + 8 <= dim; i += 8) {
        const __m256 q = LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 a = LoadAbsMagnitude8Avx2(code_abs + i);
        dot0 = _mm256_fmadd_ps(q, a, dot0);
        bias0 = _mm256_add_ps(bias0, q);
    }

    float result = HorizAdd(_mm256_add_ps(dot0, dot1));
    result += 0.5f * HorizAdd(_mm256_add_ps(bias0, bias1));

    for (; i < dim; i++) {
        const float signed_q = sign[i] ? query[i] : -query[i];
        result += signed_q * static_cast<float>(code_abs[i]) + 0.5f * signed_q;
    }
#ifndef NDEBUG
    const float ref = IPExRaBitQReference(query, code_abs, sign, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
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
    return IPExRaBitQReference(query, code_abs, sign, dim);
}

#endif  // VDB_USE_AVX2

}  // namespace simd
}  // namespace vdb
