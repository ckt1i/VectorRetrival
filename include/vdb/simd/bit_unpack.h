#pragma once

#include <cstdint>
#include "vdb/common/macros.h"

namespace vdb {
namespace simd {

/// Unpack `count` values of `bit_width` bits each from a packed byte array
/// into a uint32_t output array.
///
/// Bit packing convention: **LSB-first within each byte**.
/// Element 0 occupies bits 0..(bit_width-1) of byte 0,
/// element 1 occupies bits bit_width..(2*bit_width-1), and so on.
///
/// Supported bit widths: 1, 2, 4, 8.
///
/// Dispatch (compile-time):
///   - bit_width == 1, VDB_USE_AVX2: AVX2 specialized path.
///     Broadcasts each byte into a __m256i, applies _mm256_srlv_epi32 with
///     shift vector [0..7], masks with 1. Processes 8 output values per byte.
///     This is the critical path for RaBitQ 1-bit quantization.
///     See UNDO.txt [PHASE3-002] for deferred 2/4/8-bit AVX2 paths.
///   - All other combinations: scalar BitUnpackScalar (correct for any width).
///
/// @param packed     Packed input (ceil(count * bit_width / 8) bytes required)
/// @param bit_width  Bits per value: must be 1, 2, 4, or 8
/// @param out        Output array (at least `count` elements)
/// @param count      Number of values to unpack
void BitUnpack(const uint8_t* VDB_RESTRICT packed,
               uint8_t                      bit_width,
               uint32_t* VDB_RESTRICT       out,
               uint32_t                     count);

/// Specialized 1-bit unpack for exactly `count` values.
///
/// This is a convenience wrapper over BitUnpack with bit_width=1 hardcoded,
/// designed for the RaBitQ 1-bit quantization hot path. The primary use case
/// is count=64 (one AddressBlock), but any count is supported.
///
/// On AVX2 builds this dispatches to the same BitUnpack1Bit_AVX2 fast path:
///   - Each byte broadcasts to __m256i
///   - _mm256_srlv_epi32 with [0..7] shift vector isolates each bit
///   - _mm256_and_si256(v, 1) masks to 0/1
///   - 8 uint32_t written per byte, scalar tail for count % 8
///
/// @param packed  Packed 1-bit data (at least ceil(count/8) bytes)
/// @param out     Output array (at least `count` elements, each 0 or 1)
/// @param count   Number of 1-bit values to unpack
VDB_FORCE_INLINE void BitUnpack1(const uint8_t* VDB_RESTRICT packed,
                                  uint32_t* VDB_RESTRICT       out,
                                  uint32_t                     count) {
    BitUnpack(packed, 1u, out, count);
}

/// Overload for the default AddressBlock size (64 values).
///
/// Equivalent to `BitUnpack1(packed, out, 64)`.
/// This zero-overhead wrapper makes call sites self-documenting:
///   `BitUnpack1(block.packed_sizes, decoded_sizes);`
VDB_FORCE_INLINE void BitUnpack1(const uint8_t* VDB_RESTRICT packed,
                                  uint32_t* VDB_RESTRICT       out) {
    BitUnpack(packed, 1u, out, 64u);
}

}  // namespace simd
}  // namespace vdb
