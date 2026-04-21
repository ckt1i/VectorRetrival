#include "vdb/simd/ip_exrabitq.h"

#include <cmath>
#include <cstring>

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

VDB_FORCE_INLINE float IPExRaBitQReference(const float* VDB_RESTRICT query,
                                           const uint8_t* VDB_RESTRICT code_abs,
                                           const uint8_t* VDB_RESTRICT sign,
                                           bool sign_packed,
                                           Dim dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        const bool positive = sign_packed
            ? ((sign[i / 8] >> (i % 8)) & 1u) != 0
            : sign[i] != 0;
        const float signed_q = positive ? query[i] : -query[i];
        sum += signed_q * (static_cast<float>(code_abs[i]) + 0.5f);
    }
    return sum;
}

#if defined(VDB_USE_AVX512)
VDB_FORCE_INLINE uint64_t LoadPackedSignChunk64(const uint8_t* VDB_RESTRICT packed_sign,
                                                uint32_t bit_offset,
                                                Dim dim) {
    const uint32_t total_bytes = (dim + 7) / 8;
    const uint32_t byte_idx = bit_offset / 8;
    uint64_t bits = 0;
    if (byte_idx < total_bytes) {
        const uint32_t remaining = total_bytes - byte_idx;
        const uint32_t copy_bytes = remaining >= 8 ? 8 : remaining;
        std::memcpy(&bits, packed_sign + byte_idx, copy_bytes);
    }
    return bits;
}

VDB_FORCE_INLINE __m512 FlipQuery16PackedChunkAvx512(
    __m512 query_block,
    uint64_t sign_chunk,
    uint32_t chunk_lane_idx) {
    const uint32_t shift = chunk_lane_idx * 16u;
    const __mmask16 pos_mask =
        static_cast<__mmask16>((sign_chunk >> shift) & 0xFFFFu);
    const __mmask16 neg_mask = static_cast<__mmask16>((~pos_mask) & 0xFFFFu);
    const __m512i sign_bit = _mm512_set1_epi32(static_cast<int>(0x80000000u));
    const __m512i sign_flip = _mm512_maskz_mov_epi32(neg_mask, sign_bit);
    return _mm512_castsi512_ps(
        _mm512_xor_si512(_mm512_castps_si512(query_block), sign_flip));
}

VDB_FORCE_INLINE __m512 LoadSignedQuery16PackedChunkAvx512(
    const float* VDB_RESTRICT query,
    uint64_t sign_chunk,
    uint32_t chunk_lane_idx) {
    return FlipQuery16PackedChunkAvx512(
        _mm512_loadu_ps(query), sign_chunk, chunk_lane_idx);
}

VDB_FORCE_INLINE __m512 LoadAbsMagnitude16Avx512(
    const uint8_t* VDB_RESTRICT code_abs) {
    const __m128i codes_16 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(code_abs));
    const __m512i codes_32 = _mm512_cvtepu8_epi32(codes_16);
    return _mm512_cvtepi32_ps(codes_32);
}

VDB_FORCE_INLINE float IPExRaBitQPackedSignAvx512(
    const float* VDB_RESTRICT query,
    const uint8_t* VDB_RESTRICT code_abs,
    const uint8_t* VDB_RESTRICT packed_sign,
    Dim dim) {
    __m512 dot = _mm512_setzero_ps();
    __m512 bias = _mm512_setzero_ps();
    uint32_t i = 0;

    for (; i + 64 <= dim; i += 64) {
        const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign, i, dim);
        for (uint32_t block = 0; block < 4; ++block) {
            const __m512 q = LoadSignedQuery16PackedChunkAvx512(
                query + i + block * 16u, sign_chunk, block);
            const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i + block * 16u);
            dot = _mm512_fmadd_ps(q, a, dot);
            bias = _mm512_add_ps(bias, q);
        }
    }

    for (; i + 32 <= dim; i += 32) {
        const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign, i, dim);
        for (uint32_t block = 0; block < 2; ++block) {
            const __m512 q = LoadSignedQuery16PackedChunkAvx512(
                query + i + block * 16u, sign_chunk, block);
            const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i + block * 16u);
            dot = _mm512_fmadd_ps(q, a, dot);
            bias = _mm512_add_ps(bias, q);
        }
    }

    for (; i + 16 <= dim; i += 16) {
        const uint64_t sign_chunk = LoadPackedSignChunk64(packed_sign, i, dim);
        const __m512 q = LoadSignedQuery16PackedChunkAvx512(query + i, sign_chunk, 0);
        const __m512 a = LoadAbsMagnitude16Avx512(code_abs + i);
        dot = _mm512_fmadd_ps(q, a, dot);
        bias = _mm512_add_ps(bias, q);
    }

    float result = _mm512_reduce_add_ps(dot) + 0.5f * _mm512_reduce_add_ps(bias);
    for (; i < dim; ++i) {
        const bool positive = ((packed_sign[i / 8] >> (i % 8)) & 1u) != 0;
        const float signed_q = positive ? query[i] : -query[i];
        result += signed_q * static_cast<float>(code_abs[i]) + 0.5f * signed_q;
    }
    return result;
}

VDB_FORCE_INLINE void IPExRaBitQBatchPackedSignAvx512(
    const float* VDB_RESTRICT query,
    const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
    const uint8_t* const* VDB_RESTRICT packed_sign_ptrs,
    uint32_t count,
    Dim dim,
    float* VDB_RESTRICT out_ip_raw) {
    __m512 dot[8];
    __m512 bias[8];
    for (uint32_t c = 0; c < count; ++c) {
        dot[c] = _mm512_setzero_ps();
        bias[c] = _mm512_setzero_ps();
    }

    uint32_t i = 0;
    for (; i + 64 <= dim; i += 64) {
        const __m512 q0 = _mm512_loadu_ps(query + i);
        const __m512 q1 = _mm512_loadu_ps(query + i + 16);
        const __m512 q2 = _mm512_loadu_ps(query + i + 32);
        const __m512 q3 = _mm512_loadu_ps(query + i + 48);
        for (uint32_t c = 0; c < count; ++c) {
            const uint64_t sign_chunk =
                LoadPackedSignChunk64(packed_sign_ptrs[c], i, dim);
            const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
            const __m512 sq1 = FlipQuery16PackedChunkAvx512(q1, sign_chunk, 1);
            const __m512 sq2 = FlipQuery16PackedChunkAvx512(q2, sign_chunk, 2);
            const __m512 sq3 = FlipQuery16PackedChunkAvx512(q3, sign_chunk, 3);
            const uint8_t* const code_abs = code_abs_ptrs[c] + i;
            dot[c] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(code_abs), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq1, LoadAbsMagnitude16Avx512(code_abs + 16), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq2, LoadAbsMagnitude16Avx512(code_abs + 32), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq3, LoadAbsMagnitude16Avx512(code_abs + 48), dot[c]);
            bias[c] = _mm512_add_ps(bias[c], sq0);
            bias[c] = _mm512_add_ps(bias[c], sq1);
            bias[c] = _mm512_add_ps(bias[c], sq2);
            bias[c] = _mm512_add_ps(bias[c], sq3);
        }
    }

    for (; i + 32 <= dim; i += 32) {
        const __m512 q0 = _mm512_loadu_ps(query + i);
        const __m512 q1 = _mm512_loadu_ps(query + i + 16);
        for (uint32_t c = 0; c < count; ++c) {
            const uint64_t sign_chunk =
                LoadPackedSignChunk64(packed_sign_ptrs[c], i, dim);
            const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
            const __m512 sq1 = FlipQuery16PackedChunkAvx512(q1, sign_chunk, 1);
            const uint8_t* const code_abs = code_abs_ptrs[c] + i;
            dot[c] = _mm512_fmadd_ps(sq0, LoadAbsMagnitude16Avx512(code_abs), dot[c]);
            dot[c] = _mm512_fmadd_ps(sq1, LoadAbsMagnitude16Avx512(code_abs + 16), dot[c]);
            bias[c] = _mm512_add_ps(bias[c], sq0);
            bias[c] = _mm512_add_ps(bias[c], sq1);
        }
    }

    for (; i + 16 <= dim; i += 16) {
        const __m512 q0 = _mm512_loadu_ps(query + i);
        for (uint32_t c = 0; c < count; ++c) {
            const uint64_t sign_chunk =
                LoadPackedSignChunk64(packed_sign_ptrs[c], i, dim);
            const __m512 sq0 = FlipQuery16PackedChunkAvx512(q0, sign_chunk, 0);
            dot[c] = _mm512_fmadd_ps(
                sq0, LoadAbsMagnitude16Avx512(code_abs_ptrs[c] + i), dot[c]);
            bias[c] = _mm512_add_ps(bias[c], sq0);
        }
    }

    for (uint32_t c = 0; c < count; ++c) {
        float result = _mm512_reduce_add_ps(dot[c]) +
                       0.5f * _mm512_reduce_add_ps(bias[c]);
        for (uint32_t t = i; t < dim; ++t) {
            const bool positive =
                ((packed_sign_ptrs[c][t / 8] >> (t % 8)) & 1u) != 0;
            const float signed_q = positive ? query[t] : -query[t];
            result += signed_q * static_cast<float>(code_abs_ptrs[c][t]) + 0.5f * signed_q;
        }
        out_ip_raw[c] = result;
    }
}
#endif

#if defined(VDB_USE_AVX2)
alignas(32) const __m256i* PackedSignFlipLutAvx2() {
    alignas(32) static __m256i lut[256];
    static bool initialized = false;
    if (!initialized) {
        const uint32_t sign_bit = 0x80000000u;
        for (uint32_t bits = 0; bits < 256; ++bits) {
            lut[bits] = _mm256_setr_epi32(
                (bits & (1u << 0)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 1)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 2)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 3)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 4)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 5)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 6)) ? 0 : static_cast<int>(sign_bit),
                (bits & (1u << 7)) ? 0 : static_cast<int>(sign_bit));
        }
        initialized = true;
    }
    return lut;
}

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

VDB_FORCE_INLINE __m256 LoadSignedQuery8Avx2Packed(
    const float* VDB_RESTRICT query,
    const uint8_t* VDB_RESTRICT packed_sign,
    uint32_t bit_offset) {
    const __m256 q = _mm256_loadu_ps(query);
    const uint32_t byte_idx = bit_offset / 8;
    const __m256i sign_flip = PackedSignFlipLutAvx2()[packed_sign[byte_idx]];
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

VDB_FORCE_INLINE float IPExRaBitQGenericAvx2(const float* VDB_RESTRICT query,
                                             const uint8_t* VDB_RESTRICT code_abs,
                                             const uint8_t* VDB_RESTRICT sign,
                                             bool sign_packed,
                                             Dim dim) {
    __m256 dot0 = _mm256_setzero_ps();
    __m256 dot1 = _mm256_setzero_ps();
    __m256 bias0 = _mm256_setzero_ps();
    __m256 bias1 = _mm256_setzero_ps();
    uint32_t i = 0;

    for (; i + 32 <= dim; i += 32) {
        const __m256 q0 = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i, sign, i)
            : LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 q1 = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i + 8, sign, i + 8)
            : LoadSignedQuery8Avx2(query + i + 8, sign + i + 8);
        const __m256 q2 = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i + 16, sign, i + 16)
            : LoadSignedQuery8Avx2(query + i + 16, sign + i + 16);
        const __m256 q3 = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i + 24, sign, i + 24)
            : LoadSignedQuery8Avx2(query + i + 24, sign + i + 24);
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
        const __m256 q0 = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i, sign, i)
            : LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 q1 = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i + 8, sign, i + 8)
            : LoadSignedQuery8Avx2(query + i + 8, sign + i + 8);
        const __m256 a0 = LoadAbsMagnitude8Avx2(code_abs + i);
        const __m256 a1 = LoadAbsMagnitude8Avx2(code_abs + i + 8);
        dot0 = _mm256_fmadd_ps(q0, a0, dot0);
        dot1 = _mm256_fmadd_ps(q1, a1, dot1);
        bias0 = _mm256_add_ps(bias0, q0);
        bias1 = _mm256_add_ps(bias1, q1);
    }

    for (; i + 8 <= dim; i += 8) {
        const __m256 q = sign_packed
            ? LoadSignedQuery8Avx2Packed(query + i, sign, i)
            : LoadSignedQuery8Avx2(query + i, sign + i);
        const __m256 a = LoadAbsMagnitude8Avx2(code_abs + i);
        dot0 = _mm256_fmadd_ps(q, a, dot0);
        bias0 = _mm256_add_ps(bias0, q);
    }

    float result = HorizAdd(_mm256_add_ps(dot0, dot1));
    result += 0.5f * HorizAdd(_mm256_add_ps(bias0, bias1));
    for (; i < dim; ++i) {
        const bool positive = sign_packed
            ? ((sign[i / 8] >> (i % 8)) & 1u) != 0
            : sign[i] != 0;
        const float signed_q = positive ? query[i] : -query[i];
        result += signed_q * static_cast<float>(code_abs[i]) + 0.5f * signed_q;
    }
    return result;
}
#endif

}  // namespace

float IPExRaBitQPackedSign(const float* VDB_RESTRICT query,
                           const uint8_t* VDB_RESTRICT code_abs,
                           const uint8_t* VDB_RESTRICT packed_sign,
                           Dim dim) {
#if defined(VDB_USE_AVX512)
    float result = IPExRaBitQPackedSignAvx512(query, code_abs, packed_sign, dim);
#elif defined(VDB_USE_AVX2)
    float result = IPExRaBitQGenericAvx2(query, code_abs, packed_sign, true, dim);
#else
    float result = IPExRaBitQReference(query, code_abs, packed_sign, true, dim);
#endif
#ifndef NDEBUG
    const float ref = IPExRaBitQReference(query, code_abs, packed_sign, true, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
    return result;
}

void IPExRaBitQBatchPackedSign(const float* VDB_RESTRICT query,
                               const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
                               const uint8_t* const* VDB_RESTRICT packed_sign_ptrs,
                               uint32_t count,
                               Dim dim,
                               float* VDB_RESTRICT out_ip_raw) {
#if defined(VDB_USE_AVX512)
    IPExRaBitQBatchPackedSignAvx512(
        query, code_abs_ptrs, packed_sign_ptrs, count, dim, out_ip_raw);
#else
    for (uint32_t i = 0; i < count; ++i) {
        out_ip_raw[i] = IPExRaBitQPackedSign(
            query, code_abs_ptrs[i], packed_sign_ptrs[i], dim);
    }
#endif
#ifndef NDEBUG
    for (uint32_t i = 0; i < count; ++i) {
        const float fast = IPExRaBitQPackedSign(
            query, code_abs_ptrs[i], packed_sign_ptrs[i], dim);
        const float ref = IPExRaBitQReference(
            query, code_abs_ptrs[i], packed_sign_ptrs[i], true, dim);
        if (std::abs(fast - out_ip_raw[i]) > 1e-3f ||
            std::abs(ref - out_ip_raw[i]) > 1e-3f) {
            __builtin_trap();
        }
    }
#endif
}

float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 bool sign_packed,
                 Dim dim) {
    float result;
    if (sign_packed) {
        result = IPExRaBitQPackedSign(query, code_abs, sign, dim);
    } else {
#if defined(VDB_USE_AVX2)
        result = IPExRaBitQGenericAvx2(query, code_abs, sign, false, dim);
#else
        result = IPExRaBitQReference(query, code_abs, sign, false, dim);
#endif
    }
#ifndef NDEBUG
    const float ref = IPExRaBitQReference(query, code_abs, sign, sign_packed, dim);
    if (std::abs(ref - result) > 1e-3f) {
        __builtin_trap();
    }
#endif
    return result;
}

}  // namespace simd
}  // namespace vdb
