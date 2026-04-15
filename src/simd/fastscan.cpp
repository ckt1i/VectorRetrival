#include "vdb/simd/fastscan.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

#if defined(VDB_USE_AVX512)
VDB_FORCE_INLINE float ReduceMax512(__m512 v) {
    alignas(64) float lanes[16];
    _mm512_store_ps(lanes, v);
    float vmax = lanes[0];
    for (int i = 1; i < 16; ++i) {
        vmax = std::max(vmax, lanes[i]);
    }
    return vmax;
}
#endif

#if defined(VDB_USE_AVX2)
VDB_FORCE_INLINE float ReduceMax256(__m256 v) {
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, v);
    float vmax = lanes[0];
    for (int i = 1; i < 8; ++i) {
        vmax = std::max(vmax, lanes[i]);
    }
    return vmax;
}
#endif

}  // namespace

// ============================================================================
// QuantizeQuery14Bit — 14-bit symmetric quantization
// ============================================================================

float QuantizeQuery14Bit(const float* VDB_RESTRICT query,
                          int16_t* VDB_RESTRICT quant_out,
                          Dim dim) {
    constexpr int BQ = 14;
    constexpr float max_val = static_cast<float>((1 << (BQ - 1)) - 1);  // 8191

    // Find max absolute value
    float vmax = 0.0f;
#if defined(VDB_USE_AVX512)
    {
        const __m512 sign_mask = _mm512_set1_ps(-0.0f);
        __m512 vmax_v = _mm512_setzero_ps();
        uint32_t i = 0;
        for (; i + 16 <= dim; i += 16) {
            __m512 q = _mm512_loadu_ps(query + i);
            __m512 abs_q = _mm512_andnot_ps(sign_mask, q);
            vmax_v = _mm512_max_ps(vmax_v, abs_q);
        }
        vmax = ReduceMax512(vmax_v);
        for (; i < dim; ++i) {
            vmax = std::max(vmax, std::abs(query[i]));
        }
    }
#elif defined(VDB_USE_AVX2)
    {
        const __m256 sign_mask = _mm256_set1_ps(-0.0f);
        __m256 vmax_v = _mm256_setzero_ps();
        uint32_t i = 0;
        for (; i + 8 <= dim; i += 8) {
            __m256 q = _mm256_loadu_ps(query + i);
            __m256 abs_q = _mm256_andnot_ps(sign_mask, q);
            vmax_v = _mm256_max_ps(vmax_v, abs_q);
        }
        vmax = ReduceMax256(vmax_v);
        for (; i < dim; ++i) {
            vmax = std::max(vmax, std::abs(query[i]));
        }
    }
#else
    for (uint32_t i = 0; i < dim; ++i) {
        vmax = std::max(vmax, std::abs(query[i]));
    }
#endif

    if (vmax < 1e-10f) {
        std::memset(quant_out, 0, static_cast<size_t>(dim) * sizeof(int16_t));
        return 1.0f;
    }

    float width = vmax / max_val;
    const float inv_width = 1.0f / width;

#if defined(VDB_USE_AVX512)
    {
        const __m512 v_inv_width = _mm512_set1_ps(inv_width);
        const __m512 v_pos_bias = _mm512_set1_ps(0.5f);
        const __m512 v_neg_bias = _mm512_set1_ps(-0.5f);
        const __m512 v_zero = _mm512_setzero_ps();
        alignas(64) int32_t tmp32[16];
        uint32_t i = 0;
        for (; i + 16 <= dim; i += 16) {
            __m512 v = _mm512_loadu_ps(query + i);
            v = _mm512_mul_ps(v, v_inv_width);
            __mmask16 neg_mask = _mm512_cmp_ps_mask(v, v_zero, _CMP_LT_OQ);
            __m512 bias = _mm512_mask_blend_ps(neg_mask, v_pos_bias, v_neg_bias);
            __m512 adjusted = _mm512_add_ps(v, bias);
            __m512i q32 = _mm512_cvttps_epi32(adjusted);
            _mm512_store_si512(tmp32, q32);
            for (uint32_t j = 0; j < 16; ++j) {
                quant_out[i + j] = static_cast<int16_t>(tmp32[j]);
            }
        }
        for (; i < dim; ++i) {
            float tmp = query[i] * inv_width;
            quant_out[i] = static_cast<int16_t>(tmp + 0.5f - (tmp < 0.0f));
        }
    }
#elif defined(VDB_USE_AVX2)
    {
        const __m256 v_inv_width = _mm256_set1_ps(inv_width);
        const __m256 v_pos_bias = _mm256_set1_ps(0.5f);
        const __m256 v_neg_bias = _mm256_set1_ps(-0.5f);
        const __m256 v_zero = _mm256_setzero_ps();
        alignas(32) int32_t tmp32[8];
        uint32_t i = 0;
        for (; i + 8 <= dim; i += 8) {
            __m256 v = _mm256_loadu_ps(query + i);
            v = _mm256_mul_ps(v, v_inv_width);
            __m256 neg_mask = _mm256_cmp_ps(v, v_zero, _CMP_LT_OQ);
            __m256 bias = _mm256_blendv_ps(v_pos_bias, v_neg_bias, neg_mask);
            __m256 adjusted = _mm256_add_ps(v, bias);
            __m256i q32 = _mm256_cvttps_epi32(adjusted);
            _mm256_store_si256(reinterpret_cast<__m256i*>(tmp32), q32);
            for (uint32_t j = 0; j < 8; ++j) {
                quant_out[i + j] = static_cast<int16_t>(tmp32[j]);
            }
        }
        for (; i < dim; ++i) {
            float tmp = query[i] * inv_width;
            quant_out[i] = static_cast<int16_t>(tmp + 0.5f - (tmp < 0.0f));
        }
    }
#else
    for (uint32_t i = 0; i < dim; ++i) {
        float tmp = query[i] / width;
        // Round to nearest integer (toward-zero for negative)
        quant_out[i] = static_cast<int16_t>(tmp + 0.5f - (tmp < 0.0f));
    }
#endif

    return width;
}

// ============================================================================
// BuildFastScanLUT — build VPSHUFB LUT from quantized query
// ============================================================================

int32_t BuildFastScanLUT(const int16_t* VDB_RESTRICT quant_query,
                          uint8_t* VDB_RESTRICT lut_out,
                          Dim dim) {
    const uint32_t M = dim >> 2;  // number of 4-dim groups
    int32_t total_shift = 0;

    // Layout depends on SIMD register width:
    //   AVX-512: 4 sub-quantizers per 128-byte block (64 lo + 64 hi)
    //   AVX2:    2 sub-quantizers per 64-byte block  (32 lo + 32 hi)
#if defined(VDB_USE_AVX512)
    constexpr size_t n_lut_per_iter = 4;      // 512/128
    constexpr size_t n_code_per_iter = 128;    // 2 * 512/8
    constexpr size_t n_code_per_lane = 16;     // 128/8
    constexpr size_t hi_offset = 64;           // 512/8
#else
    constexpr size_t n_lut_per_iter = 2;      // 256/128
    constexpr size_t n_code_per_iter = 64;    // 2 * 256/8
    constexpr size_t n_code_per_lane = 16;    // 128/8
    constexpr size_t hi_offset = 32;          // 256/8
#endif

    const int16_t* qq = quant_query;
    for (uint32_t i = 0; i < M; ++i) {
        const int q0 = qq[0];
        const int q1 = qq[1];
        const int q2 = qq[2];
        const int q3 = qq[3];

        // Minimum subset sum over {q0,q1,q2,q3}: include all negative values,
        // exclude all positive values.
        const int v_min =
            std::min(q0, 0) + std::min(q1, 0) +
            std::min(q2, 0) + std::min(q3, 0);
        const int bias = -v_min;

        const uint16_t lut0  = static_cast<uint16_t>(bias);
        const uint16_t lut1  = static_cast<uint16_t>(bias + q3);
        const uint16_t lut2  = static_cast<uint16_t>(bias + q2);
        const uint16_t lut3  = static_cast<uint16_t>(bias + q2 + q3);
        const uint16_t lut4  = static_cast<uint16_t>(bias + q1);
        const uint16_t lut5  = static_cast<uint16_t>(bias + q1 + q3);
        const uint16_t lut6  = static_cast<uint16_t>(bias + q1 + q2);
        const uint16_t lut7  = static_cast<uint16_t>(bias + q1 + q2 + q3);
        const uint16_t lut8  = static_cast<uint16_t>(bias + q0);
        const uint16_t lut9  = static_cast<uint16_t>(bias + q0 + q3);
        const uint16_t lut10 = static_cast<uint16_t>(bias + q0 + q2);
        const uint16_t lut11 = static_cast<uint16_t>(bias + q0 + q2 + q3);
        const uint16_t lut12 = static_cast<uint16_t>(bias + q0 + q1);
        const uint16_t lut13 = static_cast<uint16_t>(bias + q0 + q1 + q3);
        const uint16_t lut14 = static_cast<uint16_t>(bias + q0 + q1 + q2);
        const uint16_t lut15 = static_cast<uint16_t>(bias + q0 + q1 + q2 + q3);

        uint8_t* fill_lo = lut_out + (i / n_lut_per_iter) * n_code_per_iter +
                           (i % n_lut_per_iter) * n_code_per_lane;
        uint8_t* fill_hi = fill_lo + hi_offset;

        fill_lo[0]  = static_cast<uint8_t>(lut0 & 0xFF);
        fill_lo[1]  = static_cast<uint8_t>(lut1 & 0xFF);
        fill_lo[2]  = static_cast<uint8_t>(lut2 & 0xFF);
        fill_lo[3]  = static_cast<uint8_t>(lut3 & 0xFF);
        fill_lo[4]  = static_cast<uint8_t>(lut4 & 0xFF);
        fill_lo[5]  = static_cast<uint8_t>(lut5 & 0xFF);
        fill_lo[6]  = static_cast<uint8_t>(lut6 & 0xFF);
        fill_lo[7]  = static_cast<uint8_t>(lut7 & 0xFF);
        fill_lo[8]  = static_cast<uint8_t>(lut8 & 0xFF);
        fill_lo[9]  = static_cast<uint8_t>(lut9 & 0xFF);
        fill_lo[10] = static_cast<uint8_t>(lut10 & 0xFF);
        fill_lo[11] = static_cast<uint8_t>(lut11 & 0xFF);
        fill_lo[12] = static_cast<uint8_t>(lut12 & 0xFF);
        fill_lo[13] = static_cast<uint8_t>(lut13 & 0xFF);
        fill_lo[14] = static_cast<uint8_t>(lut14 & 0xFF);
        fill_lo[15] = static_cast<uint8_t>(lut15 & 0xFF);

        fill_hi[0]  = static_cast<uint8_t>(lut0 >> 8);
        fill_hi[1]  = static_cast<uint8_t>(lut1 >> 8);
        fill_hi[2]  = static_cast<uint8_t>(lut2 >> 8);
        fill_hi[3]  = static_cast<uint8_t>(lut3 >> 8);
        fill_hi[4]  = static_cast<uint8_t>(lut4 >> 8);
        fill_hi[5]  = static_cast<uint8_t>(lut5 >> 8);
        fill_hi[6]  = static_cast<uint8_t>(lut6 >> 8);
        fill_hi[7]  = static_cast<uint8_t>(lut7 >> 8);
        fill_hi[8]  = static_cast<uint8_t>(lut8 >> 8);
        fill_hi[9]  = static_cast<uint8_t>(lut9 >> 8);
        fill_hi[10] = static_cast<uint8_t>(lut10 >> 8);
        fill_hi[11] = static_cast<uint8_t>(lut11 >> 8);
        fill_hi[12] = static_cast<uint8_t>(lut12 >> 8);
        fill_hi[13] = static_cast<uint8_t>(lut13 >> 8);
        fill_hi[14] = static_cast<uint8_t>(lut14 >> 8);
        fill_hi[15] = static_cast<uint8_t>(lut15 >> 8);

        total_shift += v_min;
        qq += 4;
    }

    return total_shift;
}

// ============================================================================
// AccumulateBlock — VPSHUFB batch-32 accumulation
// ============================================================================

#if defined(VDB_USE_AVX512)

void AccumulateBlock(const uint8_t* VDB_RESTRICT packed_codes,
                     const uint8_t* VDB_RESTRICT lut,
                     uint32_t* VDB_RESTRICT result,
                     Dim dim) {
    const uint32_t M = dim >> 2;
    const __m512i lo_mask = _mm512_set1_epi8(0x0f);

    // Two-plane accumulation: [plane][lo_vec/hi_shifted/lo_vec_hi/hi_shifted_hi]
    __m512i accu[2][4];
    for (int p = 0; p < 2; ++p)
        for (int a = 0; a < 4; ++a)
            accu[p][a] = _mm512_setzero_si512();

    // Process 4 sub-quantizers per iteration (64 bytes codes, 2x64 bytes LUT)
    for (uint32_t m = 0; m < M; m += 4) {
        __m512i c = _mm512_loadu_si512(packed_codes);
        __m512i lo = _mm512_and_si512(c, lo_mask);
        __m512i hi = _mm512_and_si512(_mm512_srli_epi16(c, 4), lo_mask);

        // Two LUT planes (lo byte + hi byte)
        for (int p = 0; p < 2; ++p) {
            __m512i lut_vec = _mm512_load_si512(lut);

            __m512i res_lo = _mm512_shuffle_epi8(lut_vec, lo);
            __m512i res_hi = _mm512_shuffle_epi8(lut_vec, hi);

            accu[p][0] = _mm512_add_epi16(accu[p][0], res_lo);
            accu[p][1] = _mm512_add_epi16(accu[p][1], _mm512_srli_epi16(res_lo, 8));

            accu[p][2] = _mm512_add_epi16(accu[p][2], res_hi);
            accu[p][3] = _mm512_add_epi16(accu[p][3], _mm512_srli_epi16(res_hi, 8));

            lut += 64;
        }
        packed_codes += 64;
    }

    // Reduce and combine: 32 results = 2 x 16
    __m512i res[2];    // final 32 uint32_t
    __m512i dis0[2], dis1[2];

    for (int p = 0; p < 2; ++p) {
        // Low 16 vectors (from lo-nibble path)
        __m256i tmp0 = _mm256_add_epi16(
            _mm512_castsi512_si256(accu[p][0]),
            _mm512_extracti64x4_epi64(accu[p][0], 1));
        __m256i tmp1 = _mm256_add_epi16(
            _mm512_castsi512_si256(accu[p][1]),
            _mm512_extracti64x4_epi64(accu[p][1], 1));
        tmp0 = _mm256_sub_epi16(tmp0, _mm256_slli_epi16(tmp1, 8));

        dis0[p] = _mm512_add_epi32(
            _mm512_cvtepu16_epi32(_mm256_permute2f128_si256(tmp0, tmp1, 0x21)),
            _mm512_cvtepu16_epi32(_mm256_blend_epi32(tmp0, tmp1, 0xF0)));

        // High 16 vectors (from hi-nibble path)
        __m256i tmp2 = _mm256_add_epi16(
            _mm512_castsi512_si256(accu[p][2]),
            _mm512_extracti64x4_epi64(accu[p][2], 1));
        __m256i tmp3 = _mm256_add_epi16(
            _mm512_castsi512_si256(accu[p][3]),
            _mm512_extracti64x4_epi64(accu[p][3], 1));
        tmp2 = _mm256_sub_epi16(tmp2, _mm256_slli_epi16(tmp3, 8));

        dis1[p] = _mm512_add_epi32(
            _mm512_cvtepu16_epi32(_mm256_permute2f128_si256(tmp2, tmp3, 0x21)),
            _mm512_cvtepu16_epi32(_mm256_blend_epi32(tmp2, tmp3, 0xF0)));
    }

    // Combine lo + (hi << 8) planes
    res[0] = _mm512_add_epi32(dis0[0], _mm512_slli_epi32(dis0[1], 8));
    res[1] = _mm512_add_epi32(dis1[0], _mm512_slli_epi32(dis1[1], 8));

    _mm512_storeu_si512(result, res[0]);
    _mm512_storeu_si512(result + 16, res[1]);
}

#elif defined(VDB_USE_AVX2)

void AccumulateBlock(const uint8_t* VDB_RESTRICT packed_codes,
                     const uint8_t* VDB_RESTRICT lut,
                     uint32_t* VDB_RESTRICT result,
                     Dim dim) {
    const uint32_t M = dim >> 2;
    const __m256i lo_mask = _mm256_set1_epi8(0x0f);

    __m256i accu[2][4];
    for (int p = 0; p < 2; ++p)
        for (int a = 0; a < 4; ++a)
            accu[p][a] = _mm256_setzero_si256();

    // Process 2 sub-quantizers per iteration (32 bytes codes, 2x32 bytes LUT)
    for (uint32_t m = 0; m < M; m += 2) {
        __m256i c = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(packed_codes));
        __m256i lo = _mm256_and_si256(c, lo_mask);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(c, 4), lo_mask);

        for (int p = 0; p < 2; ++p) {
            __m256i lut_vec = _mm256_load_si256(
                reinterpret_cast<const __m256i*>(lut));

            __m256i res_lo = _mm256_shuffle_epi8(lut_vec, lo);
            __m256i res_hi = _mm256_shuffle_epi8(lut_vec, hi);

            accu[p][0] = _mm256_add_epi16(accu[p][0], res_lo);
            accu[p][1] = _mm256_add_epi16(accu[p][1], _mm256_srli_epi16(res_lo, 8));

            accu[p][2] = _mm256_add_epi16(accu[p][2], res_hi);
            accu[p][3] = _mm256_add_epi16(accu[p][3], _mm256_srli_epi16(res_hi, 8));

            lut += 32;
        }
        packed_codes += 32;
    }

    // Reduce: same logic as AVX-512 but stays in 256-bit
    for (int p = 0; p < 2; ++p) {
        accu[p][0] = _mm256_sub_epi16(accu[p][0],
                                       _mm256_slli_epi16(accu[p][1], 8));
        accu[p][2] = _mm256_sub_epi16(accu[p][2],
                                       _mm256_slli_epi16(accu[p][3], 8));
    }

    // Extract 32 uint16 results, expand to uint32, combine planes
    // Low 16 vectors
    alignas(32) uint16_t raw0[2][16];
    alignas(32) uint16_t raw1[2][16];
    for (int p = 0; p < 2; ++p) {
        __m256i d0 = _mm256_add_epi16(
            _mm256_permute2f128_si256(accu[p][0], accu[p][1], 0x21),
            _mm256_blend_epi32(accu[p][0], accu[p][1], 0xF0));
        _mm256_store_si256(reinterpret_cast<__m256i*>(raw0[p]), d0);

        __m256i d1 = _mm256_add_epi16(
            _mm256_permute2f128_si256(accu[p][2], accu[p][3], 0x21),
            _mm256_blend_epi32(accu[p][2], accu[p][3], 0xF0));
        _mm256_store_si256(reinterpret_cast<__m256i*>(raw1[p]), d1);
    }

    for (int v = 0; v < 16; ++v) {
        result[v] = static_cast<uint32_t>(raw0[0][v]) +
                    (static_cast<uint32_t>(raw0[1][v]) << 8);
    }
    for (int v = 0; v < 16; ++v) {
        result[16 + v] = static_cast<uint32_t>(raw1[0][v]) +
                         (static_cast<uint32_t>(raw1[1][v]) << 8);
    }
}

#else  // Scalar fallback

void AccumulateBlock(const uint8_t* VDB_RESTRICT packed_codes,
                     const uint8_t* VDB_RESTRICT lut,
                     uint32_t* VDB_RESTRICT result,
                     Dim dim) {
    // Scalar: decode nibbles and look up in LUT
    const uint32_t M = dim >> 2;
    std::memset(result, 0, 32 * sizeof(uint32_t));

    for (uint32_t m = 0; m < M; m += 2) {
        // In AVX2 layout: 2 sub-quantizers per 64-byte block
        const uint8_t* lut_lo_p0 = lut + (m / 2) * 64 + 0;      // plane 0, sub m
        const uint8_t* lut_lo_p1 = lut + (m / 2) * 64 + 16;     // plane 0, sub m+1
        const uint8_t* lut_hi_p0 = lut + (m / 2) * 64 + 32;     // plane 1, sub m
        const uint8_t* lut_hi_p1 = lut + (m / 2) * 64 + 32 + 16;// plane 1, sub m+1

        const uint8_t* codes = packed_codes + (m / 2) * 32;

        for (int v = 0; v < 16; ++v) {
            uint8_t byte = codes[v];
            uint8_t lo_nib = byte & 0x0F;
            uint8_t hi_nib = (byte >> 4) & 0x0F;

            // Sub-quantizer m, vector v (low half)
            result[v] += static_cast<uint32_t>(lut_lo_p0[lo_nib]) +
                         (static_cast<uint32_t>(lut_hi_p0[lo_nib]) << 8);
            // Sub-quantizer m, vector v+16 (high half)
            result[16 + v] += static_cast<uint32_t>(lut_lo_p0[hi_nib]) +
                              (static_cast<uint32_t>(lut_hi_p0[hi_nib]) << 8);
        }
        for (int v = 0; v < 16; ++v) {
            uint8_t byte = codes[16 + v];
            uint8_t lo_nib = byte & 0x0F;
            uint8_t hi_nib = (byte >> 4) & 0x0F;

            // Sub-quantizer m+1, vector v
            result[v] += static_cast<uint32_t>(lut_lo_p1[lo_nib]) +
                         (static_cast<uint32_t>(lut_hi_p1[lo_nib]) << 8);
            // Sub-quantizer m+1, vector v+16
            result[16 + v] += static_cast<uint32_t>(lut_lo_p1[hi_nib]) +
                              (static_cast<uint32_t>(lut_hi_p1[hi_nib]) << 8);
        }
    }
}

#endif  // VDB_USE_AVX512 / VDB_USE_AVX2 / scalar

// ============================================================================
// FastScanSafeOutMask — batch SafeOut classification for one FastScan block
// ============================================================================

uint32_t FastScanSafeOutMask(const float* VDB_RESTRICT dists,
                              const float* VDB_RESTRICT block_norms,
                              uint32_t count,
                              float est_kth,
                              float margin_factor) {
#if defined(VDB_USE_AVX512)
    const __m512 v_mfac    = _mm512_set1_ps(margin_factor);
    const __m512 v_est_kth = _mm512_set1_ps(est_kth);
    const __m512 v_two     = _mm512_set1_ps(2.0f);

    uint32_t result = 0;

    for (uint32_t base = 0; base < count; base += 16) {
        // Build a lane mask covering only the valid lanes in this 16-wide chunk
        uint32_t valid = std::min<uint32_t>(16u, count - base);
        __mmask16 lane_mask = (valid >= 16)
            ? __mmask16(0xFFFF)
            : __mmask16((1u << valid) - 1u);

        // Load dists / norms (use masked loads for the tail to avoid OOB read)
        __m512 v_dists, v_norms;
        if (valid >= 16) {
            v_dists = _mm512_loadu_ps(dists + base);
            v_norms = _mm512_loadu_ps(block_norms + base);
        } else {
            v_dists = _mm512_maskz_loadu_ps(lane_mask, dists + base);
            v_norms = _mm512_maskz_loadu_ps(lane_mask, block_norms + base);
        }

        // so_thresh = est_kth + 2 * margin_factor * norm
        __m512 v_margin = _mm512_mul_ps(v_mfac, v_norms);
        __m512 v_so_thr = _mm512_fmadd_ps(v_two, v_margin, v_est_kth);
        // bit = (dist > so_thresh) within valid lanes
        __mmask16 so_mask = _mm512_mask_cmp_ps_mask(
            lane_mask, v_dists, v_so_thr, _CMP_GT_OQ);
        result |= static_cast<uint32_t>(so_mask) << base;
    }
    return result;
#else
    uint32_t result = 0;
    for (uint32_t v = 0; v < count; ++v) {
        float so_thresh = est_kth + 2.0f * margin_factor * block_norms[v];
        if (dists[v] > so_thresh) {
            result |= 1u << v;
        }
    }
    return result;
#endif
}

// ============================================================================
// FastScanDequantize — convert raw_accu into final L2² distances
// ============================================================================

void FastScanDequantize(const uint32_t* VDB_RESTRICT raw_accu,
                        const float* VDB_RESTRICT block_norms,
                        uint32_t count,
                        int32_t fs_shift,
                        float fs_width,
                        float sum_q,
                        float inv_sqrt_dim,
                        float norm_qc,
                        float norm_qc_sq,
                        float* VDB_RESTRICT out_dist) {
#if defined(VDB_USE_AVX512)
    // AVX-512 path: 16 lanes per iteration. For typical count=32 → 2 iterations.
    const __m512 v_shift     = _mm512_set1_ps(static_cast<float>(fs_shift));
    const __m512 v_width     = _mm512_set1_ps(fs_width);
    const __m512 v_sum_q     = _mm512_set1_ps(sum_q);
    const __m512 v_inv_sqrt  = _mm512_set1_ps(inv_sqrt_dim);
    const __m512 v_norm_qc   = _mm512_set1_ps(norm_qc);
    const __m512 v_norm_qc_sq= _mm512_set1_ps(norm_qc_sq);
    const __m512 v_two       = _mm512_set1_ps(2.0f);
    const __m512 v_zero      = _mm512_setzero_ps();

    uint32_t v = 0;
    for (; v + 16 <= count; v += 16) {
        // raw_accu uint32 -> float
        __m512i v_raw_i = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(raw_accu + v));
        __m512  v_raw   = _mm512_cvtepu32_ps(v_raw_i);
        // ip_raw = (raw + shift) * width
        __m512  v_ip_raw = _mm512_mul_ps(_mm512_add_ps(v_raw, v_shift), v_width);
        // ip_est = (2*ip_raw - sum_q) * inv_sqrt_dim
        __m512  v_ip_est = _mm512_mul_ps(
            _mm512_fmsub_ps(v_two, v_ip_raw, v_sum_q), v_inv_sqrt);
        // norm_oc = block_norms[v]
        __m512  v_norm_oc = _mm512_loadu_ps(block_norms + v);
        // dist = norm_oc² + norm_qc_sq
        __m512  v_dist = _mm512_fmadd_ps(v_norm_oc, v_norm_oc, v_norm_qc_sq);
        // dist -= 2*norm_oc*norm_qc*ip_est
        __m512  v_two_oc_qc = _mm512_mul_ps(v_two, _mm512_mul_ps(v_norm_oc, v_norm_qc));
        v_dist = _mm512_fnmadd_ps(v_two_oc_qc, v_ip_est, v_dist);
        // max(dist, 0)
        v_dist = _mm512_max_ps(v_dist, v_zero);
        _mm512_storeu_ps(out_dist + v, v_dist);
    }
    // Scalar tail
    for (; v < count; ++v) {
        float ip_raw = (static_cast<float>(raw_accu[v]) +
                        static_cast<float>(fs_shift)) * fs_width;
        float ip_est = (2.0f * ip_raw - sum_q) * inv_sqrt_dim;
        float dist_sq = block_norms[v] * block_norms[v] + norm_qc_sq
                        - 2.0f * block_norms[v] * norm_qc * ip_est;
        out_dist[v] = std::max(dist_sq, 0.0f);
    }
#else
    // Scalar fallback
    for (uint32_t v = 0; v < count; ++v) {
        float ip_raw = (static_cast<float>(raw_accu[v]) +
                        static_cast<float>(fs_shift)) * fs_width;
        float ip_est = (2.0f * ip_raw - sum_q) * inv_sqrt_dim;
        float dist_sq = block_norms[v] * block_norms[v] + norm_qc_sq
                        - 2.0f * block_norms[v] * norm_qc * ip_est;
        out_dist[v] = std::max(dist_sq, 0.0f);
    }
#endif
}

}  // namespace simd
}  // namespace vdb
