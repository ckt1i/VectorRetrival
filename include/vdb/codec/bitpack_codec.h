#pragma once

#include <cstdint>
#include <vector>

namespace vdb {
namespace codec {

/// Bit-packing codec for arrays of uint32_t values.
///
/// All values in one array are packed with the **same** fixed bit width,
/// matching the on-disk format of the Address Column's size fields:
///   block.sizes[64]  packed as `bit_width × 64` bits = `bit_width * 8` bytes.
///
/// Bit packing convention: **LSB-first within each byte** (matches BitUnpack).
///
/// Decode delegates to vdb::simd::BitUnpack for AVX2 acceleration on 1-bit.
/// Encode is scalar (offline construction path — not on the query hot path).
/// See UNDO.txt [PHASE3-004] for deferred SIMD encode optimization.
class BitpackCodec {
public:
    BitpackCodec() = delete;  // Static-only utility class

    /// Default threshold for extreme-value filtering.
    ///
    /// If max(values) > max_packable_value, ComputeMinBitWidth returns 0 to
    /// indicate that bit-packing is not worthwhile (the caller should store
    /// raw uint32_t instead).
    ///
    /// Default = 256, which means values need at most 8 bits (bit_width ≤ 8).
    /// This aligns with 1 MB / 4 KB = 256, matching the Address Column design
    /// where record sizes within one block are expected to be small.
    static constexpr uint32_t kDefaultMaxPackableValue = 256u;

    /// Compute the minimum number of bits needed to represent all values.
    ///
    /// Returns at least 1 bit even if max_value == 0.
    /// The result satisfies: all values fit in [0, 2^return_value - 1].
    ///
    /// If any value > max_packable_value, returns **0** as a sentinel meaning
    /// "do not bit-pack these values". The caller should then store the raw
    /// uint32_t array directly.
    ///
    /// @param values             Array of values to inspect
    /// @param count              Number of values (0 → returns 1)
    /// @param max_packable_value Upper bound; values above this → return 0.
    ///                           Pass UINT32_MAX to disable the check.
    static uint8_t ComputeMinBitWidth(const uint32_t* values,
                                      uint32_t        count,
                                      uint32_t        max_packable_value = kDefaultMaxPackableValue);

    /// Return the number of bytes required to pack `count` values at `bit_width`
    /// bits each: ceil(count * bit_width / 8).
    static uint32_t EncodedSize(uint32_t count, uint8_t bit_width) noexcept {
        return (static_cast<uint64_t>(count) * bit_width + 7u) / 8u;
    }

    /// Pack `count` values from `values[]` into a byte array using `bit_width`
    /// bits per value (LSB-first, values must fit in bit_width bits).
    ///
    /// @param values     Input array (length >= count)
    /// @param count      Number of values to encode
    /// @param bit_width  Bits per value (1, 2, 4, or 8)
    /// @return           Packed bytes (length == EncodedSize(count, bit_width))
    static std::vector<uint8_t> Encode(const uint32_t* values,
                                       uint32_t        count,
                                       uint8_t         bit_width);

    /// Unpack `count` values from `packed[]` into a uint32_t vector.
    /// Delegates to vdb::simd::BitUnpack (AVX2 for bit_width=1).
    ///
    /// @param packed     Packed input bytes (>= EncodedSize(count, bit_width))
    /// @param bit_width  Bits per value (1, 2, 4, or 8)
    /// @param count      Number of values to decode
    /// @return           Decoded uint32_t values (length == count)
    static std::vector<uint32_t> Decode(const uint8_t* packed,
                                        uint8_t        bit_width,
                                        uint32_t       count);
};

}  // namespace codec
}  // namespace vdb
