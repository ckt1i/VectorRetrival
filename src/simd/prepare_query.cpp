#include "vdb/simd/prepare_query.h"

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

#if defined(VDB_USE_AVX512)
VDB_FORCE_INLINE float ReduceAdd512(__m512 v) {
    return _mm512_reduce_add_ps(v);
}
#endif

#if defined(VDB_USE_AVX2)
VDB_FORCE_INLINE float ReduceAdd256(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}
#endif

}  // namespace

float SimdSubtractAndNormSq(const float* VDB_RESTRICT a,
                            const float* VDB_RESTRICT b,
                            float* VDB_RESTRICT out,
                            uint32_t dim) {
#if defined(VDB_USE_AVX512)
    __m512 acc = _mm512_setzero_ps();
    uint32_t i = 0;

    if (b != nullptr) {
        for (; i + 16 <= dim; i += 16) {
            __m512 va = _mm512_loadu_ps(a + i);
            __m512 vb = _mm512_loadu_ps(b + i);
            __m512 diff = _mm512_sub_ps(va, vb);
            _mm512_storeu_ps(out + i, diff);
            acc = _mm512_fmadd_ps(diff, diff, acc);
        }
    } else {
        for (; i + 16 <= dim; i += 16) {
            __m512 va = _mm512_loadu_ps(a + i);
            _mm512_storeu_ps(out + i, va);
            acc = _mm512_fmadd_ps(va, va, acc);
        }
    }

    float result = ReduceAdd512(acc);
#elif defined(VDB_USE_AVX2)
    __m256 acc = _mm256_setzero_ps();
    uint32_t i = 0;

    if (b != nullptr) {
        for (; i + 8 <= dim; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            __m256 vb = _mm256_loadu_ps(b + i);
            __m256 diff = _mm256_sub_ps(va, vb);
            _mm256_storeu_ps(out + i, diff);
            acc = _mm256_fmadd_ps(diff, diff, acc);
        }
    } else {
        for (; i + 8 <= dim; i += 8) {
            __m256 va = _mm256_loadu_ps(a + i);
            _mm256_storeu_ps(out + i, va);
            acc = _mm256_fmadd_ps(va, va, acc);
        }
    }

    float result = ReduceAdd256(acc);
#else
    uint32_t i = 0;
    float result = 0.0f;
#endif

    if (b != nullptr) {
        for (; i < dim; ++i) {
            float diff = a[i] - b[i];
            out[i] = diff;
            result += diff * diff;
        }
    } else {
        for (; i < dim; ++i) {
            out[i] = a[i];
            result += a[i] * a[i];
        }
    }
    return result;
}

float SimdNormalizeSignSum(float* VDB_RESTRICT vec,
                           float inv_norm,
                           uint64_t* sign_code_words,
                           uint32_t num_words,
                           uint32_t dim) {
#if defined(VDB_USE_AVX512)
    __m512 vscale = _mm512_set1_ps(inv_norm);
    __m512 vzero = _mm512_setzero_ps();
    __m512 vsum = _mm512_setzero_ps();
    uint32_t i = 0;

    for (; i + 16 <= dim; i += 16) {
        __m512 v = _mm512_loadu_ps(vec + i);
        v = _mm512_mul_ps(v, vscale);
        _mm512_storeu_ps(vec + i, v);
        vsum = _mm512_add_ps(vsum, v);

        const __mmask16 mask = _mm512_cmp_ps_mask(v, vzero, _CMP_GE_OQ);
        const uint32_t word_idx = i / 64;
        const uint32_t bit_off = i % 64;
        sign_code_words[word_idx] |= (static_cast<uint64_t>(mask) << bit_off);
        if (bit_off + 16 > 64 && word_idx + 1 < num_words) {
            sign_code_words[word_idx + 1] |=
                (static_cast<uint64_t>(mask) >> (64 - bit_off));
        }
    }

    float result = ReduceAdd512(vsum);
#elif defined(VDB_USE_AVX2)
    const __m256 vscale = _mm256_set1_ps(inv_norm);
    const __m256 vzero = _mm256_setzero_ps();
    __m256 vsum = _mm256_setzero_ps();
    uint32_t i = 0;

    for (; i + 8 <= dim; i += 8) {
        __m256 v = _mm256_loadu_ps(vec + i);
        v = _mm256_mul_ps(v, vscale);
        _mm256_storeu_ps(vec + i, v);
        vsum = _mm256_add_ps(vsum, v);

        const __m256 ge = _mm256_cmp_ps(v, vzero, _CMP_GE_OQ);
        const uint64_t mask = static_cast<uint64_t>(_mm256_movemask_ps(ge));
        const uint32_t word_idx = i / 64;
        const uint32_t bit_off = i % 64;
        sign_code_words[word_idx] |= (mask << bit_off);
        if (bit_off + 8 > 64 && word_idx + 1 < num_words) {
            sign_code_words[word_idx + 1] |= (mask >> (64 - bit_off));
        }
    }

    float result = ReduceAdd256(vsum);
#else
    uint32_t i = 0;
    float result = 0.0f;
#endif

    for (; i < dim; ++i) {
        vec[i] *= inv_norm;
        result += vec[i];
        if (vec[i] >= 0.0f) {
            const uint32_t word_idx = i / 64;
            const uint32_t bit_idx = i % 64;
            sign_code_words[word_idx] |= (1ULL << bit_idx);
        }
    }

    return result;
}

}  // namespace simd
}  // namespace vdb
