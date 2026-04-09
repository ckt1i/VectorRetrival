#pragma once

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

#include <cstdint>

#if defined(VDB_USE_AVX512)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

// ============================================================================
// SimdSubtractAndNormSq
//   Computes out[i] = a[i] - b[i] for all i, and returns Σ out[i]².
//   If b == nullptr, copies a into out and returns Σ a[i]².
//   Single memory pass: subtract and accumulate squared differences together.
// ============================================================================

#if defined(VDB_USE_AVX512)

VDB_FORCE_INLINE float SimdSubtractAndNormSq(const float* VDB_RESTRICT a,
                                              const float* VDB_RESTRICT b,
                                              float* VDB_RESTRICT out,
                                              uint32_t dim) {
    __m512 acc = _mm512_setzero_ps();
    uint32_t i = 0;

    if (b != nullptr) {
        for (; i + 16 <= dim; i += 16) {
            __m512 va   = _mm512_loadu_ps(a + i);
            __m512 vb   = _mm512_loadu_ps(b + i);
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

    float result = _mm512_reduce_add_ps(acc);

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

#else

VDB_FORCE_INLINE float SimdSubtractAndNormSq(const float* VDB_RESTRICT a,
                                              const float* VDB_RESTRICT b,
                                              float* VDB_RESTRICT out,
                                              uint32_t dim) {
    float result = 0.0f;
    if (b != nullptr) {
        for (uint32_t i = 0; i < dim; ++i) {
            float diff = a[i] - b[i];
            out[i] = diff;
            result += diff * diff;
        }
    } else {
        for (uint32_t i = 0; i < dim; ++i) {
            out[i] = a[i];
            result += a[i] * a[i];
        }
    }
    return result;
}

#endif  // VDB_USE_AVX512

// ============================================================================
// SimdNormalizeSignSum
//   In a single pass over vec[0..dim):
//   - Multiplies each element in-place by inv_norm (normalize)
//   - Packs sign bits into sign_code_words (1 = element >= 0)
//   - Accumulates the sum of all (scaled) elements
//   Returns Σ (vec[i] * inv_norm).
//   sign_code_words must be zeroed by the caller before calling.
// ============================================================================

#if defined(VDB_USE_AVX512)

VDB_FORCE_INLINE float SimdNormalizeSignSum(float* VDB_RESTRICT vec,
                                             float inv_norm,
                                             uint64_t* sign_code_words,
                                             uint32_t num_words,
                                             uint32_t dim) {
    __m512 vscale = _mm512_set1_ps(inv_norm);
    __m512 vzero  = _mm512_setzero_ps();
    __m512 vsum   = _mm512_setzero_ps();
    uint32_t i = 0;

    for (; i + 16 <= dim; i += 16) {
        __m512 v    = _mm512_loadu_ps(vec + i);
        v           = _mm512_mul_ps(v, vscale);
        _mm512_storeu_ps(vec + i, v);
        vsum        = _mm512_add_ps(vsum, v);

        // Build sign bits: 1 where v[lane] >= 0
        __mmask16 mask = _mm512_cmp_ps_mask(v, vzero, _CMP_GE_OQ);

        // Pack the 16-bit mask into the appropriate word(s)
        // Bits i..i+15 map to word i/64 at bit positions i%64..i%64+15
        uint32_t word_idx = i / 64;
        uint32_t bit_off  = i % 64;
        sign_code_words[word_idx] |= (static_cast<uint64_t>(mask) << bit_off);
        // Handle wrap-around when bit_off + 16 > 64
        if (bit_off + 16 > 64 && word_idx + 1 < num_words) {
            sign_code_words[word_idx + 1] |=
                (static_cast<uint64_t>(mask) >> (64 - bit_off));
        }
    }

    float result = _mm512_reduce_add_ps(vsum);

    for (; i < dim; ++i) {
        vec[i] *= inv_norm;
        result += vec[i];
        if (vec[i] >= 0.0f) {
            uint32_t word_idx = i / 64;
            uint32_t bit_idx  = i % 64;
            sign_code_words[word_idx] |= (1ULL << bit_idx);
        }
    }

    return result;
}

#else

VDB_FORCE_INLINE float SimdNormalizeSignSum(float* VDB_RESTRICT vec,
                                             float inv_norm,
                                             uint64_t* sign_code_words,
                                             uint32_t /*num_words*/,
                                             uint32_t dim) {
    float result = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        vec[i] *= inv_norm;
        result += vec[i];
        if (vec[i] >= 0.0f) {
            uint32_t word_idx = i / 64;
            uint32_t bit_idx  = i % 64;
            sign_code_words[word_idx] |= (1ULL << bit_idx);
        }
    }
    return result;
}

#endif  // VDB_USE_AVX512

}  // namespace simd
}  // namespace vdb
