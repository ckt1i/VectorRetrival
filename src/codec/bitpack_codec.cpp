#include "vdb/codec/bitpack_codec.h"

#include <algorithm>
#include <cstring>

#include "vdb/simd/bit_unpack.h"

namespace vdb {
namespace codec {

uint8_t BitpackCodec::ComputeMinBitWidth(const uint32_t* values,
                                          uint32_t        count,
                                          uint32_t        max_packable_value) {
    if (count == 0u) return 1u;

    uint32_t max_val = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        if (values[i] > max_val) max_val = values[i];
    }

    // Extreme-value check: if any value exceeds the threshold, bit-packing
    // is not worthwhile. Return 0 as a sentinel → caller stores raw uint32_t.
    if (max_val > max_packable_value) return 0u;

    // Count bits needed: smallest b such that 2^b > max_val.
    // Special case: max_val == 0 → 1 bit minimum.
    if (max_val == 0u) return 1u;

    uint8_t bits = 0u;
    uint32_t shifted = max_val;
    while (shifted != 0u) {
        shifted >>= 1;
        bits++;
    }
    return bits;  // 2^bits > max_val, i.e. values fit in `bits` bits
}

std::vector<uint8_t> BitpackCodec::Encode(const uint32_t* values,
                                           uint32_t        count,
                                           uint8_t         bit_width) {
    const uint32_t total_bytes = EncodedSize(count, bit_width);
    std::vector<uint8_t> packed(total_bytes, 0u);

    // Scalar LSB-first bit packing — handles arbitrary bit_width up to 32.
    // Each value is written across as many byte boundaries as needed.
    uint32_t bit_pos = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t val            = values[i];
        uint32_t bits_remaining = bit_width;
        uint32_t cur_bit        = bit_pos;

        while (bits_remaining > 0u) {
            uint32_t byte_idx = cur_bit >> 3;
            uint32_t bit_off  = cur_bit & 7u;
            uint32_t space    = 8u - bit_off;  // bits available in this byte
            uint32_t to_write = std::min(space, bits_remaining);

            uint8_t mask = static_cast<uint8_t>((1u << to_write) - 1u);
            packed[byte_idx] |= static_cast<uint8_t>((val & mask) << bit_off);

            val >>= to_write;
            cur_bit += to_write;
            bits_remaining -= to_write;
        }

        bit_pos += bit_width;
    }

    return packed;
}

std::vector<uint32_t> BitpackCodec::Decode(const uint8_t* packed,
                                            uint8_t        bit_width,
                                            uint32_t       count) {
    std::vector<uint32_t> out(count);
    if (count > 0u) {
        simd::BitUnpack(packed, bit_width, out.data(), count);
    }
    return out;
}

}  // namespace codec
}  // namespace vdb
