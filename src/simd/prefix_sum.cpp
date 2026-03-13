#include "vdb/simd/prefix_sum.h"

#ifdef VDB_USE_AVX2
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

// ============================================================================
// Scalar fallback
// ============================================================================

void ExclusivePrefixSumScalar(const uint32_t* VDB_RESTRICT in,
                               uint32_t* VDB_RESTRICT       out,
                               uint32_t                     count) {
    uint32_t running = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        out[i]   = running;
        running += in[i];
    }
}

// ============================================================================
// AVX2 implementation (8 uint32_t per register, intra-lane prefix + carry)
// ============================================================================
#ifdef VDB_USE_AVX2

/// Compute the **inclusive** prefix sum of 8 uint32_t packed in one __m256i.
///
/// Steps:
///   1. _mm256_slli_si256(v, 4)  — shift bytes left by 4 within each 128-bit
///      lane (= shift by 1 uint32_t); adds neighbor to the right.
///   2. _mm256_slli_si256(v, 8)  — shift by 8 bytes (2 uint32_t); adds 2-ahead.
///      After steps 1-2, each lane independently holds its inclusive prefix.
///   3. Extract the total of lane 0 (element [3]) and add it to lane 1.
///
/// Result: inclusive[i] = in[0] + in[1] + ... + in[i]
VDB_FORCE_INLINE __m256i IncPrefixSum8(__m256i v) {
    // Step 1: add right-neighbor within each 128-bit lane
    v = _mm256_add_epi32(v, _mm256_slli_si256(v, 4));
    // Step 2: add 2-ahead within each 128-bit lane
    v = _mm256_add_epi32(v, _mm256_slli_si256(v, 8));

    // Step 3: cross-lane carry — add lane-0 total to all elements of lane-1
    __m128i lo       = _mm256_castsi256_si128(v);
    int32_t lo_total = _mm_extract_epi32(lo, 3);    // inclusive[3]
    __m128i hi       = _mm256_extracti128_si256(v, 1);
    hi = _mm_add_epi32(hi, _mm_set1_epi32(lo_total));

    return _mm256_inserti128_si256(v, hi, 1);
}

/// Convert an inclusive prefix sum __m256i + running_sum into an exclusive
/// prefix-sum __m256i, using _mm_alignr_epi8 for cross-lane element shift.
///
/// _mm_alignr_epi8(a, b, 12):
///   Concatenates 16 bytes: [a (high) || b (low)], shifts right by 12 bytes,
///   returns the low 16 bytes.
///   In uint32_t terms: result = [b[3], a[0], a[1], a[2]].
///
/// So:
///   new_lo = _mm_alignr_epi8(lo_inc, zeros,  12) = [0,        inc[0], inc[1], inc[2]]
///   new_hi = _mm_alignr_epi8(hi_inc, lo_inc, 12) = [inc[3],   inc[4], inc[5], inc[6]]
///
/// Adding running_sum to both halves gives the exclusive prefix sum for this
/// 8-element block.
VDB_FORCE_INLINE __m256i IncToExcl8(__m256i inc, uint32_t running_sum) {
    __m128i lo_inc = _mm256_castsi256_si128(inc);
    __m128i hi_inc = _mm256_extracti128_si256(inc, 1);

    __m128i new_lo = _mm_alignr_epi8(lo_inc, _mm_setzero_si128(), 12);
    __m128i new_hi = _mm_alignr_epi8(hi_inc, lo_inc, 12);

    __m128i carry  = _mm_set1_epi32(static_cast<int32_t>(running_sum));
    new_lo = _mm_add_epi32(new_lo, carry);
    new_hi = _mm_add_epi32(new_hi, carry);

    return _mm256_set_m128i(new_hi, new_lo);
}

void ExclusivePrefixSumAVX2(const uint32_t* VDB_RESTRICT in,
                              uint32_t* VDB_RESTRICT       out,
                              uint32_t                     count) {
    uint32_t running_sum = 0u;
    uint32_t i           = 0u;

    for (; i + 8u <= count; i += 8u) {
        __m256i v   = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
        __m256i inc = IncPrefixSum8(v);
        __m256i ex  = IncToExcl8(inc, running_sum);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), ex);

        // Advance running_sum by the total of this 8-element block.
        // inc[7] (element 3 of lane-1) = in[i]+...+in[i+7]
        running_sum += static_cast<uint32_t>(
            _mm_extract_epi32(_mm256_extracti128_si256(inc, 1), 3));
    }

    // Scalar tail
    for (; i < count; i++) {
        out[i]       = running_sum;
        running_sum += in[i];
    }
}

#endif  // VDB_USE_AVX2

// ============================================================================
// Multi-stream scalar fallback
// ============================================================================

void ExclusivePrefixSumMultiScalar(const uint32_t* VDB_RESTRICT interleaved_in,
                                     uint32_t* VDB_RESTRICT       interleaved_out,
                                     uint32_t                     count,
                                     uint32_t                     num_streams) {
    // K independent running sums, one per stream
    uint32_t running[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (uint32_t j = 0; j < count; ++j) {
        for (uint32_t k = 0; k < num_streams; ++k) {
            interleaved_out[j * 8 + k] = running[k];
            running[k] += interleaved_in[j * 8 + k];
        }
        // Zero excess lanes
        for (uint32_t k = num_streams; k < 8; ++k) {
            interleaved_out[j * 8 + k] = 0;
        }
    }
}

// ============================================================================
// Multi-stream AVX2: one _mm256_add_epi32 per element index
// ============================================================================
#ifdef VDB_USE_AVX2

void ExclusivePrefixSumMultiAVX2(const uint32_t* VDB_RESTRICT interleaved_in,
                                    uint32_t* VDB_RESTRICT       interleaved_out,
                                    uint32_t                     count,
                                    uint32_t                     num_streams) {
    VDB_UNUSED(num_streams);  // All 8 lanes processed; excess are zero → no effect
    __m256i running = _mm256_setzero_si256();

    for (uint32_t j = 0; j < count; ++j) {
        __m256i vals = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(interleaved_in + j * 8));
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(interleaved_out + j * 8), running);
        running = _mm256_add_epi32(running, vals);
    }
}

#endif  // VDB_USE_AVX2

}  // namespace

// ============================================================================
// Public entry points
// ============================================================================

void ExclusivePrefixSum(const uint32_t* VDB_RESTRICT in,
                        uint32_t* VDB_RESTRICT       out,
                        uint32_t                     count) {
    if (count == 0u) return;

#ifdef VDB_USE_AVX2
    ExclusivePrefixSumAVX2(in, out, count);
#else
    ExclusivePrefixSumScalar(in, out, count);
#endif
}

void ExclusivePrefixSumMulti(const uint32_t* VDB_RESTRICT interleaved_in,
                              uint32_t* VDB_RESTRICT       interleaved_out,
                              uint32_t                     count,
                              uint32_t                     num_streams) {
    if (count == 0u || num_streams == 0u) return;

#ifdef VDB_USE_AVX2
    ExclusivePrefixSumMultiAVX2(interleaved_in, interleaved_out, count, num_streams);
#else
    ExclusivePrefixSumMultiScalar(interleaved_in, interleaved_out, count, num_streams);
#endif
}

}  // namespace simd
}  // namespace vdb
