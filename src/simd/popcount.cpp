#include "vdb/simd/popcount.h"

#ifdef VDB_USE_AVX2
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

// ============================================================================
// AVX2 implementation — VPSHUFB-based popcount
// ============================================================================
#ifdef VDB_USE_AVX2

namespace {

/// VPSHUFB-based popcount for a __m256i register.
/// Uses a nibble-lookup approach: split each byte into low/high nibble,
/// look up the popcount of each nibble via pshufb, then sum.
VDB_FORCE_INLINE __m256i PopcountVec(__m256i v) {
    const __m256i lookup = _mm256_setr_epi8(
        /* 0*/ 0, 1, 1, 2, 1, 2, 2, 3,
        /* 8*/ 1, 2, 2, 3, 2, 3, 3, 4,
        /* 0*/ 0, 1, 1, 2, 1, 2, 2, 3,
        /* 8*/ 1, 2, 2, 3, 2, 3, 3, 4
    );
    const __m256i low_mask = _mm256_set1_epi8(0x0F);

    __m256i lo = _mm256_and_si256(v, low_mask);
    __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low_mask);
    __m256i cnt_lo = _mm256_shuffle_epi8(lookup, lo);
    __m256i cnt_hi = _mm256_shuffle_epi8(lookup, hi);
    return _mm256_add_epi8(cnt_lo, cnt_hi);
}

/// Sum all bytes in a __m256i into a uint32_t.
/// Uses _mm256_sad_epu8 against zero to horizontally sum bytes within
/// each 64-bit lane, then extracts and sums 4 lanes.
VDB_FORCE_INLINE uint32_t HorizSumBytes(__m256i v) {
    __m256i zero = _mm256_setzero_si256();
    __m256i sad  = _mm256_sad_epu8(v, zero);
    // sad has 4 × 64-bit values, each containing a partial byte sum
    __m128i lo = _mm256_castsi256_si128(sad);
    __m128i hi = _mm256_extracti128_si256(sad, 1);
    __m128i sum = _mm_add_epi64(lo, hi);
    // sum[0] + sum[1]
    return static_cast<uint32_t>(_mm_extract_epi64(sum, 0) +
                                 _mm_extract_epi64(sum, 1));
}

}  // namespace

uint32_t PopcountXor(const uint64_t* VDB_RESTRICT a,
                     const uint64_t* VDB_RESTRICT b,
                     uint32_t num_words) {
    uint32_t total = 0;
    uint32_t i = 0;

    // Main loop: 4 uint64_t (256 bits) per iteration
    // Accumulate byte-level popcounts in an accumulator register.
    // Flush every 31 iterations to avoid byte overflow (max 8*4=32 per iter,
    // 31*32 = 992 < 255*... actually each byte ≤ 8, 31*8=248 < 255).
    __m256i acc = _mm256_setzero_si256();
    uint32_t batch_count = 0;

    for (; i + 4 <= num_words; i += 4) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i xored = _mm256_xor_si256(va, vb);
        acc = _mm256_add_epi8(acc, PopcountVec(xored));

        ++batch_count;
        if (batch_count == 31) {
            total += HorizSumBytes(acc);
            acc = _mm256_setzero_si256();
            batch_count = 0;
        }
    }

    total += HorizSumBytes(acc);

    // Scalar tail
    for (; i < num_words; ++i) {
        total += Popcount64(a[i] ^ b[i]);
    }

    return total;
}

uint32_t PopcountTotal(const uint64_t* code, uint32_t num_words) {
    uint32_t total = 0;
    uint32_t i = 0;

    __m256i acc = _mm256_setzero_si256();
    uint32_t batch_count = 0;

    for (; i + 4 <= num_words; i += 4) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(code + i));
        acc = _mm256_add_epi8(acc, PopcountVec(v));

        ++batch_count;
        if (batch_count == 31) {
            total += HorizSumBytes(acc);
            acc = _mm256_setzero_si256();
            batch_count = 0;
        }
    }

    total += HorizSumBytes(acc);

    for (; i < num_words; ++i) {
        total += Popcount64(code[i]);
    }

    return total;
}

// ============================================================================
// Scalar fallback
// ============================================================================
#else

uint32_t PopcountXor(const uint64_t* VDB_RESTRICT a,
                     const uint64_t* VDB_RESTRICT b,
                     uint32_t num_words) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < num_words; ++i) {
        total += Popcount64(a[i] ^ b[i]);
    }
    return total;
}

uint32_t PopcountTotal(const uint64_t* code, uint32_t num_words) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < num_words; ++i) {
        total += Popcount64(code[i]);
    }
    return total;
}

#endif  // VDB_USE_AVX2

}  // namespace simd
}  // namespace vdb
