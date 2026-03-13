#include "vdb/simd/transpose.h"

#include <cstring>

#ifdef VDB_USE_AVX2
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

// ============================================================================
// Scalar fallback
// ============================================================================

void Transpose8xNScalar(const uint32_t* const* streams_in,
                         uint32_t*             interleaved_out,
                         uint32_t              num_streams,
                         uint32_t              count_per_stream) {
    for (uint32_t j = 0; j < count_per_stream; ++j) {
        uint32_t k = 0;
        for (; k < num_streams; ++k) {
            interleaved_out[j * 8 + k] = streams_in[k][j];
        }
        for (; k < 8; ++k) {
            interleaved_out[j * 8 + k] = 0;
        }
    }
}

void TransposeNx8Scalar(const uint32_t*  interleaved_in,
                          uint32_t* const* streams_out,
                          uint32_t         num_streams,
                          uint32_t         count_per_stream) {
    for (uint32_t j = 0; j < count_per_stream; ++j) {
        for (uint32_t k = 0; k < num_streams; ++k) {
            streams_out[k][j] = interleaved_in[j * 8 + k];
        }
    }
}

// ============================================================================
// AVX2 implementation: 8×8 sub-block transpose
// ============================================================================
#ifdef VDB_USE_AVX2

/// Transpose an 8×8 block of uint32_t values.
///
/// Input:  8 __m256i registers, each containing 8 uint32_t from one row.
/// Output: 8 __m256i registers, transposed so each contains 8 uint32_t from
///         one column of the original.
///
/// Algorithm uses unpacklo/hi_epi32, then unpacklo/hi_epi64, then
/// permute2x128 to perform a full 8×8 transpose in 24 instructions.
VDB_FORCE_INLINE void Transpose8x8AVX2(__m256i& r0, __m256i& r1,
                                         __m256i& r2, __m256i& r3,
                                         __m256i& r4, __m256i& r5,
                                         __m256i& r6, __m256i& r7) {
    // Phase 1: interleave 32-bit elements
    __m256i t0 = _mm256_unpacklo_epi32(r0, r1);  // a0b0 a1b1 | a4b4 a5b5
    __m256i t1 = _mm256_unpackhi_epi32(r0, r1);  // a2b2 a3b3 | a6b6 a7b7
    __m256i t2 = _mm256_unpacklo_epi32(r2, r3);
    __m256i t3 = _mm256_unpackhi_epi32(r2, r3);
    __m256i t4 = _mm256_unpacklo_epi32(r4, r5);
    __m256i t5 = _mm256_unpackhi_epi32(r4, r5);
    __m256i t6 = _mm256_unpacklo_epi32(r6, r7);
    __m256i t7 = _mm256_unpackhi_epi32(r6, r7);

    // Phase 2: interleave 64-bit elements
    __m256i u0 = _mm256_unpacklo_epi64(t0, t2);
    __m256i u1 = _mm256_unpackhi_epi64(t0, t2);
    __m256i u2 = _mm256_unpacklo_epi64(t1, t3);
    __m256i u3 = _mm256_unpackhi_epi64(t1, t3);
    __m256i u4 = _mm256_unpacklo_epi64(t4, t6);
    __m256i u5 = _mm256_unpackhi_epi64(t4, t6);
    __m256i u6 = _mm256_unpacklo_epi64(t5, t7);
    __m256i u7 = _mm256_unpackhi_epi64(t5, t7);

    // Phase 3: permute 128-bit lanes
    r0 = _mm256_permute2x128_si256(u0, u4, 0x20);
    r1 = _mm256_permute2x128_si256(u1, u5, 0x20);
    r2 = _mm256_permute2x128_si256(u2, u6, 0x20);
    r3 = _mm256_permute2x128_si256(u3, u7, 0x20);
    r4 = _mm256_permute2x128_si256(u0, u4, 0x31);
    r5 = _mm256_permute2x128_si256(u1, u5, 0x31);
    r6 = _mm256_permute2x128_si256(u2, u6, 0x31);
    r7 = _mm256_permute2x128_si256(u3, u7, 0x31);
}

void Transpose8xNAVX2(const uint32_t* const* streams_in,
                        uint32_t*             interleaved_out,
                        uint32_t              num_streams,
                        uint32_t              count_per_stream) {
    // Process 8-element chunks using 8×8 sub-block transpose
    uint32_t j = 0;
    for (; j + 8 <= count_per_stream; j += 8) {
        __m256i r0, r1, r2, r3, r4, r5, r6, r7;
        // Load 8 elements from each stream (or zeros for inactive streams)
        r0 = (num_streams > 0) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[0] + j)) : _mm256_setzero_si256();
        r1 = (num_streams > 1) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[1] + j)) : _mm256_setzero_si256();
        r2 = (num_streams > 2) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[2] + j)) : _mm256_setzero_si256();
        r3 = (num_streams > 3) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[3] + j)) : _mm256_setzero_si256();
        r4 = (num_streams > 4) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[4] + j)) : _mm256_setzero_si256();
        r5 = (num_streams > 5) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[5] + j)) : _mm256_setzero_si256();
        r6 = (num_streams > 6) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[6] + j)) : _mm256_setzero_si256();
        r7 = (num_streams > 7) ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(streams_in[7] + j)) : _mm256_setzero_si256();

        Transpose8x8AVX2(r0, r1, r2, r3, r4, r5, r6, r7);

        // Store 8 interleaved rows (each row is 8 uint32_t = one __m256i)
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 0) * 8), r0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 1) * 8), r1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 2) * 8), r2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 3) * 8), r3);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 4) * 8), r4);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 5) * 8), r5);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 6) * 8), r6);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(interleaved_out + (j + 7) * 8), r7);
    }

    // Scalar tail for remaining elements
    for (; j < count_per_stream; ++j) {
        uint32_t k = 0;
        for (; k < num_streams; ++k) {
            interleaved_out[j * 8 + k] = streams_in[k][j];
        }
        for (; k < 8; ++k) {
            interleaved_out[j * 8 + k] = 0;
        }
    }
}

void TransposeNx8AVX2(const uint32_t*  interleaved_in,
                        uint32_t* const* streams_out,
                        uint32_t         num_streams,
                        uint32_t         count_per_stream) {
    uint32_t j = 0;
    for (; j + 8 <= count_per_stream; j += 8) {
        // Load 8 interleaved rows
        __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 0) * 8));
        __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 1) * 8));
        __m256i r2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 2) * 8));
        __m256i r3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 3) * 8));
        __m256i r4 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 4) * 8));
        __m256i r5 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 5) * 8));
        __m256i r6 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 6) * 8));
        __m256i r7 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(interleaved_in + (j + 7) * 8));

        // Transpose: interleaved→SoA
        Transpose8x8AVX2(r0, r1, r2, r3, r4, r5, r6, r7);

        // Store to each stream
        __m256i* rows[8] = {&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7};
        for (uint32_t k = 0; k < num_streams; ++k) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(streams_out[k] + j), *rows[k]);
        }
    }

    // Scalar tail
    for (; j < count_per_stream; ++j) {
        for (uint32_t k = 0; k < num_streams; ++k) {
            streams_out[k][j] = interleaved_in[j * 8 + k];
        }
    }
}

#endif  // VDB_USE_AVX2

}  // namespace

// ============================================================================
// Public entry points
// ============================================================================

void Transpose8xN(const uint32_t* const* streams_in,
                   uint32_t*             interleaved_out,
                   uint32_t              num_streams,
                   uint32_t              count_per_stream) {
    if (count_per_stream == 0 || num_streams == 0) return;

#ifdef VDB_USE_AVX2
    Transpose8xNAVX2(streams_in, interleaved_out, num_streams, count_per_stream);
#else
    Transpose8xNScalar(streams_in, interleaved_out, num_streams, count_per_stream);
#endif
}

void TransposeNx8(const uint32_t*  interleaved_in,
                   uint32_t* const* streams_out,
                   uint32_t         num_streams,
                   uint32_t         count_per_stream) {
    if (count_per_stream == 0 || num_streams == 0) return;

#ifdef VDB_USE_AVX2
    TransposeNx8AVX2(interleaved_in, streams_out, num_streams, count_per_stream);
#else
    TransposeNx8Scalar(interleaved_in, streams_out, num_streams, count_per_stream);
#endif
}

}  // namespace simd
}  // namespace vdb
