#include "vdb/storage/address_column.h"

#include <algorithm>
#include <cstring>

#include "vdb/codec/bitpack_codec.h"
#include "vdb/simd/bit_unpack.h"
#include "vdb/simd/prefix_sum.h"

namespace vdb {
namespace storage {

// ============================================================================
// Encode — convert AddressEntry[] into AddressBlock[]
// ============================================================================

std::vector<AddressBlock> AddressColumn::Encode(
    const std::vector<AddressEntry>& entries,
    uint32_t block_granularity,
    uint32_t page_size) {

    std::vector<AddressBlock> blocks;
    if (entries.empty()) return blocks;

    const uint32_t N = static_cast<uint32_t>(entries.size());
    const uint32_t num_blocks = (N + block_granularity - 1) / block_granularity;
    blocks.reserve(num_blocks);

    for (uint32_t b = 0; b < num_blocks; ++b) {
        uint32_t start = b * block_granularity;
        uint32_t count = std::min(block_granularity, N - start);
        blocks.push_back(EncodeSingleBlock(&entries[start], count, page_size));
    }

    return blocks;
}

AddressBlock AddressColumn::EncodeSingleBlock(const AddressEntry* entries,
                                               uint32_t count,
                                               uint32_t page_size) {
    AddressBlock block;
    block.page_size = page_size;
    block.base_offset = entries[0].offset / page_size;  // page index
    block.record_count = count;

    // Extract sizes in page units (ceil division)
    std::vector<uint32_t> sizes(count);
    for (uint32_t i = 0; i < count; ++i) {
        sizes[i] = (entries[i].size + page_size - 1) / page_size;
    }

    // Compute minimum bit width for all sizes in this block.
    // Pass UINT32_MAX to disable the max_packable_value check — we always
    // want to bit-pack (the sentinel 0 return would mean "do not pack").
    block.bit_width = codec::BitpackCodec::ComputeMinBitWidth(
        sizes.data(), count, UINT32_MAX);

    // bit_width == 0 should never happen with UINT32_MAX, but guard anyway
    if (block.bit_width == 0) block.bit_width = 1;

    // Bit-pack the sizes
    block.packed = codec::BitpackCodec::Encode(sizes.data(), count,
                                                block.bit_width);

    return block;
}

// ============================================================================
// DecodeBlock — SIMD-accelerated block decoding
// ============================================================================

void AddressColumn::DecodeBlock(const AddressBlock& block,
                                 std::vector<AddressEntry>& out_entries) {
    const uint32_t count = block.record_count;
    out_entries.resize(count);

    if (count == 0) return;

    // Step 1: bit_unpack → sizes[]
    // Allocate to full block granularity for SIMD alignment, then trim.
    const uint32_t aligned_count = ((count + 7) / 8) * 8;  // round up to 8
    std::vector<uint32_t> sizes(aligned_count, 0);
    simd::BitUnpack(block.packed.data(), block.bit_width,
                    sizes.data(), count);

    // Step 2: exclusive prefix sum → offsets[]
    std::vector<uint32_t> prefix(aligned_count, 0);
    simd::ExclusivePrefixSum(sizes.data(), prefix.data(), count);

    // Step 3: reconstruct AddressEntry values (convert page units → bytes)
    const uint64_t ps = block.page_size;
    for (uint32_t i = 0; i < count; ++i) {
        out_entries[i].offset =
            (block.base_offset + static_cast<uint64_t>(prefix[i])) * ps;
        out_entries[i].size =
            static_cast<uint32_t>(static_cast<uint64_t>(sizes[i]) * ps);
    }
}


// ============================================================================
// TotalRecords
// ============================================================================

uint32_t AddressColumn::TotalRecords(const std::vector<AddressBlock>& blocks) {
    uint32_t total = 0;
    for (const auto& b : blocks) {
        total += b.record_count;
    }
    return total;
}

}  // namespace storage
}  // namespace vdb
