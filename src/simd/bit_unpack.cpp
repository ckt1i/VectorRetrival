#include "vdb/simd/bit_unpack.h"

#ifdef VDB_USE_AVX2
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

namespace {

// ============================================================================
// Scalar fallback — correct for any bit_width in [1..8]
// ============================================================================

void BitUnpackScalar(const uint8_t* VDB_RESTRICT packed,
                     uint8_t                      bit_width,
                     uint32_t* VDB_RESTRICT       out,
                     uint32_t                     count) {
    const uint32_t mask = (bit_width < 32u) ? ((1u << bit_width) - 1u) : 0xFFFFFFFFu;
    uint32_t bit_pos = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t byte_idx = bit_pos >> 3;       // bit_pos / 8
        uint32_t bit_off  = bit_pos & 7u;       // bit_pos % 8

        // A value of up to 8 bits may straddle two bytes.
        uint32_t word = static_cast<uint32_t>(packed[byte_idx]);
        if (bit_off + bit_width > 8u) {
            word |= static_cast<uint32_t>(packed[byte_idx + 1]) << 8;
        }
        out[i] = (word >> bit_off) & mask;
        bit_pos += bit_width;
    }
}

// ============================================================================
// AVX2 specialized path for bit_width == 1
// ============================================================================
#ifdef VDB_USE_AVX2

/// Expand one packed byte (8 bits, LSB-first) into 8 uint32_t values (0 or 1).
/// Uses _mm256_srlv_epi32 for per-lane variable shift (AVX2).
///
/// For count=64 (one AddressBlock), this inner loop executes exactly 8 times.
void BitUnpack1Bit_AVX2(const uint8_t* VDB_RESTRICT packed,
                         uint32_t* VDB_RESTRICT       out,
                         uint32_t                     count) {
    // Shift amounts isolate bit i (LSB-first): element j gets bit j of the byte.
    const __m256i shifts = _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0);
    const __m256i ones   = _mm256_set1_epi32(1);

    const uint32_t full_bytes = count >> 3;   // count / 8
    for (uint32_t b = 0; b < full_bytes; b++) {
        __m256i v = _mm256_set1_epi32(static_cast<int>(packed[b]));
        v = _mm256_srlv_epi32(v, shifts);
        v = _mm256_and_si256(v, ones);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + b * 8u), v);
    }

    // Scalar tail: remaining bits that don't fill a full byte
    const uint32_t done      = full_bytes * 8u;
    const uint32_t remaining = count - done;
    if (remaining > 0u) {
        const uint32_t tail_byte = static_cast<uint32_t>(packed[full_bytes]);
        for (uint32_t bit = 0; bit < remaining; bit++) {
            out[done + bit] = (tail_byte >> bit) & 1u;
        }
    }
}

#endif  // VDB_USE_AVX2

}  // namespace

// ============================================================================
// Public entry point
// ============================================================================

void BitUnpack(const uint8_t* VDB_RESTRICT packed,
               uint8_t                      bit_width,
               uint32_t* VDB_RESTRICT       out,
               uint32_t                     count) {
    if (count == 0u) return;

#ifdef VDB_USE_AVX2
    if (bit_width == 1u) {
        BitUnpack1Bit_AVX2(packed, out, count);
        return;
    }
#endif

    // Generic scalar path for bit_width = 2, 4, 8 (and 1 without AVX2).
    // See UNDO.txt [PHASE3-002] for future AVX2 paths for other widths.
    BitUnpackScalar(packed, bit_width, out, count);
}

}  // namespace simd
}  // namespace vdb
