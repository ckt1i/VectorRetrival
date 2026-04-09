#include "vdb/simd/signed_dot.h"

#ifdef VDB_USE_AVX512
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

#ifndef VDB_USE_AVX512
// ============================================================================
// Scalar reference — matches original bit-extract + fmadd loop exactly.
// Active when AVX-512 is unavailable; becomes the dispatch target.
// ============================================================================
static float SignedDotFromBitsScalar(const float* q,
                                      const uint64_t* sign_bits,
                                      uint32_t dim) {
    float dot = 0.0f;
    for (uint32_t d = 0; d < dim; ++d) {
        int bit = static_cast<int>((sign_bits[d / 64] >> (d % 64)) & 1);
        dot += q[d] * (2.0f * static_cast<float>(bit) - 1.0f);
    }
    return dot;
}
#endif  // !VDB_USE_AVX512

#ifdef VDB_USE_AVX512

// ============================================================================
// ExtractMask16 — extract 16-bit mask from a packed uint64_t[] bit array.
//
// Supports arbitrary bit_offset (i.e. dim not required to be a multiple of
// 64). When bit_in_word > 48, the 16-bit window straddles two uint64 words.
// ============================================================================
static inline __mmask16 ExtractMask16(const uint64_t* bits,
                                       uint32_t bit_offset) {
    uint32_t word_idx   = bit_offset >> 6;        // bit_offset / 64
    uint32_t bit_in_word = bit_offset & 63;        // bit_offset % 64
    uint64_t lo = bits[word_idx] >> bit_in_word;
    if (bit_in_word > 48) {
        // High bits come from the next word
        uint64_t hi = bits[word_idx + 1] << (64u - bit_in_word);
        lo |= hi;
    }
    return static_cast<__mmask16>(lo & 0xFFFFu);
}

// ============================================================================
// AVX-512 path — 16 floats per iteration.
//
// For each 16-float chunk:
//   mask bit=1 → keep  +q[i]
//   mask bit=0 → use   -q[i]
// Then accumulate into a 512-bit register; reduce at the end.
// ============================================================================
static float SignedDotFromBitsAVX512(const float* q,
                                      const uint64_t* sign_bits,
                                      uint32_t dim) {
    __m512 acc = _mm512_setzero_ps();
    const __m512 zero = _mm512_setzero_ps();

    uint32_t d = 0;
    for (; d + 16 <= dim; d += 16) {
        __m512 qv      = _mm512_loadu_ps(q + d);
        __mmask16 mask = ExtractMask16(sign_bits, d);
        __m512 neg_qv  = _mm512_sub_ps(zero, qv);
        // blend: where mask bit=1, select qv (+q); where 0, select neg_qv (-q)
        __m512 signed_qv = _mm512_mask_blend_ps(mask, neg_qv, qv);
        acc = _mm512_add_ps(acc, signed_qv);
    }

    float result = _mm512_reduce_add_ps(acc);

    // Scalar tail for remaining elements (when dim % 16 != 0)
    for (; d < dim; ++d) {
        int bit = static_cast<int>((sign_bits[d / 64] >> (d % 64)) & 1);
        result += q[d] * (2.0f * static_cast<float>(bit) - 1.0f);
    }

    return result;
}

#endif  // VDB_USE_AVX512

}  // namespace

// ============================================================================
// Public dispatch
// ============================================================================

float SignedDotFromBits(const float* q, const uint64_t* sign_bits,
                        uint32_t dim) {
#ifdef VDB_USE_AVX512
    return SignedDotFromBitsAVX512(q, sign_bits, dim);
#else
    return SignedDotFromBitsScalar(q, sign_bits, dim);
#endif
}

}  // namespace simd
}  // namespace vdb
