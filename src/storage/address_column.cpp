#include "vdb/storage/address_column.h"

#include <algorithm>
#include <cstring>

#include "vdb/codec/bitpack_codec.h"
#include "vdb/simd/bit_unpack.h"
#include "vdb/simd/prefix_sum.h"
#include "vdb/simd/transpose.h"

namespace vdb {
namespace storage {

namespace {

uint32_t ComputeSharedBitWidth(const std::vector<AddressEntry>& entries,
                               uint32_t page_size) {
    if (entries.empty()) return 1u;

    std::vector<uint32_t> sizes(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        sizes[i] = (entries[i].size + page_size - 1) / page_size;
    }

    uint8_t bit_width = codec::BitpackCodec::ComputeMinBitWidth(
        sizes.data(), static_cast<uint32_t>(sizes.size()), UINT32_MAX);
    return bit_width == 0 ? 1u : bit_width;
}

uint32_t ComputeBlockGranularity(uint32_t fixed_packed_size, uint8_t bit_width) {
    if (fixed_packed_size == 0) return 0;
    if (bit_width == 0) return 0;

    uint32_t granularity = static_cast<uint32_t>(
        (static_cast<uint64_t>(fixed_packed_size) * 8u) / bit_width);
    return std::max<uint32_t>(1u, granularity);
}

}  // namespace

// ============================================================================
// Encode — convert AddressEntry[] into AddressBlock[]
// ============================================================================

EncodedAddressColumn AddressColumn::Encode(
    const std::vector<AddressEntry>& entries,
    uint32_t fixed_packed_size,
    uint32_t page_size) {
    EncodedAddressColumn column;
    column.layout.page_size = page_size;
    column.layout.fixed_packed_size = fixed_packed_size;
    column.total_records = static_cast<uint32_t>(entries.size());

    if (entries.empty()) {
        column.layout.bit_width = 1u;
        column.layout.block_granularity = 0u;
        column.layout.last_packed_size = 0u;
        column.layout.num_address_blocks = 0u;
        return column;
    }

    if (fixed_packed_size == 0) fixed_packed_size = kDefaultFixedPackedSize;
    column.layout.fixed_packed_size = fixed_packed_size;
    column.layout.bit_width = static_cast<uint8_t>(
        ComputeSharedBitWidth(entries, page_size));
    column.layout.block_granularity = ComputeBlockGranularity(
        fixed_packed_size, column.layout.bit_width);

    const uint32_t N = static_cast<uint32_t>(entries.size());
    const uint32_t num_blocks =
        (N + column.layout.block_granularity - 1) / column.layout.block_granularity;
    column.layout.num_address_blocks = num_blocks;
    column.blocks.reserve(num_blocks);

    for (uint32_t b = 0; b < num_blocks; ++b) {
        const uint32_t start = b * column.layout.block_granularity;
        const uint32_t count = std::min(column.layout.block_granularity, N - start);
        const bool is_last = (b + 1 == num_blocks);

        auto block = EncodeSingleBlock(
            &entries[start], count, page_size, column.layout.bit_width,
            is_last ? 0u : fixed_packed_size);

        if (is_last) {
            column.layout.last_packed_size =
                static_cast<uint32_t>(block.packed.size());
        }

        column.blocks.push_back(std::move(block));
    }

    return column;
}

AddressBlock AddressColumn::EncodeSingleBlock(const AddressEntry* entries,
                                              uint32_t count,
                                              uint32_t page_size,
                                              uint8_t bit_width,
                                              uint32_t padded_size) {
    AddressBlock block;
    block.base_offset = static_cast<uint32_t>(entries[0].offset / page_size);  // page index (u32)

    // Extract sizes in page units (ceil division)
    std::vector<uint32_t> sizes(count);
    for (uint32_t i = 0; i < count; ++i) {
        sizes[i] = (entries[i].size + page_size - 1) / page_size;
    }

    // Bit-pack the sizes
    block.packed = codec::BitpackCodec::Encode(sizes.data(), count, bit_width);

    if (padded_size > block.packed.size()) {
        block.packed.resize(padded_size, 0u);
    }

    return block;
}

// ============================================================================
// DecodeBlock — SIMD-accelerated block decoding
// ============================================================================

void AddressColumn::DecodeSingleBlock(const AddressColumnLayout& layout,
                                      const AddressBlock& block,
                                      uint32_t record_count,
                                      std::vector<AddressEntry>& out_entries) {
    const uint32_t count = record_count;
    out_entries.resize(count);

    if (count == 0) return;

    // Step 1: bit_unpack → sizes[]
    // Allocate to full block granularity for SIMD alignment, then trim.
    const uint32_t aligned_count = ((count + 7) / 8) * 8;  // round up to 8
    std::vector<uint32_t> sizes(aligned_count, 0);
    simd::BitUnpack(block.packed.data(), layout.bit_width,
                    sizes.data(), count);

    // Step 2: exclusive prefix sum → offsets[]
    std::vector<uint32_t> prefix(aligned_count, 0);
    simd::ExclusivePrefixSum(sizes.data(), prefix.data(), count);

    // Step 3: reconstruct AddressEntry values (convert page units → bytes)
    const uint64_t ps = layout.page_size;
    for (uint32_t i = 0; i < count; ++i) {
        out_entries[i].offset =
            (static_cast<uint64_t>(block.base_offset) + prefix[i]) * ps;
        out_entries[i].size =
            static_cast<uint32_t>(static_cast<uint64_t>(sizes[i]) * ps);
    }
}

Status AddressColumn::Decode(const EncodedAddressColumn& column,
                             std::vector<AddressEntry>& out_entries) {
    return DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, out_entries);
}

// ============================================================================
// DecodeMultiStream — multi-stream SIMD pipeline for regular blocks
// ============================================================================

void AddressColumn::DecodeMultiStream(const AddressColumnLayout& layout,
                                       const AddressBlock* blocks,
                                       uint32_t num_blocks,
                                       AddressEntry* out_entries) {
    if (num_blocks == 0) return;

    const uint32_t G = layout.block_granularity;
    const uint32_t K = kMultiStreamWidth;  // 8
    const uint64_t ps = layout.page_size;

    // Phase 1: BitUnpack each block into SoA buffers
    // Allocate K buffers of G elements each (aligned to 8 for SIMD)
    const uint32_t aligned_G = ((G + 7) / 8) * 8;
    std::vector<std::vector<uint32_t>> sizes_soa(K, std::vector<uint32_t>(aligned_G, 0));
    const uint32_t* soa_ptrs[8];
    uint32_t* soa_mut_ptrs[8];

    for (uint32_t k = 0; k < num_blocks; ++k) {
        simd::BitUnpack(blocks[k].packed.data(), layout.bit_width,
                        sizes_soa[k].data(), G);
        soa_ptrs[k] = sizes_soa[k].data();
        soa_mut_ptrs[k] = sizes_soa[k].data();
    }
    // Zero-fill unused streams
    for (uint32_t k = num_blocks; k < K; ++k) {
        soa_ptrs[k] = sizes_soa[k].data();  // already zeroed
        soa_mut_ptrs[k] = sizes_soa[k].data();
    }

    // Phase 2: Transpose SoA → interleaved
    std::vector<uint32_t> interleaved_sizes(G * K, 0);
    simd::Transpose8xN(soa_ptrs, interleaved_sizes.data(), num_blocks, G);

    // Phase 3: Multi-stream exclusive prefix sum
    std::vector<uint32_t> interleaved_prefix(G * K, 0);
    simd::ExclusivePrefixSumMulti(interleaved_sizes.data(),
                                   interleaved_prefix.data(), G, num_blocks);

    // Phase 4: Transpose prefix back to SoA
    std::vector<std::vector<uint32_t>> prefix_soa(K, std::vector<uint32_t>(G, 0));
    uint32_t* prefix_ptrs[8];
    for (uint32_t k = 0; k < K; ++k) {
        prefix_ptrs[k] = prefix_soa[k].data();
    }
    simd::TransposeNx8(interleaved_prefix.data(), prefix_ptrs, num_blocks, G);

    // Phase 5: Materialize AddressEntry (uint32→uint64 widening here only)
    for (uint32_t k = 0; k < num_blocks; ++k) {
        const uint32_t base = blocks[k].base_offset;
        AddressEntry* out = out_entries + static_cast<size_t>(k) * G;
        for (uint32_t j = 0; j < G; ++j) {
            out[j].offset = (static_cast<uint64_t>(base) + prefix_soa[k][j]) * ps;
            out[j].size = static_cast<uint32_t>(
                static_cast<uint64_t>(sizes_soa[k][j]) * ps);
        }
    }
}

// ============================================================================
// DecodeBatchBlocks — uses multi-stream pipeline for regular blocks
// ============================================================================

Status AddressColumn::DecodeBatchBlocks(const AddressColumnLayout& layout,
                                        const std::vector<AddressBlock>& blocks,
                                        uint32_t total_records,
                                        std::vector<AddressEntry>& out_entries) {
    if (blocks.size() != layout.num_address_blocks) {
        return Status::InvalidArgument("Address block count does not match layout");
    }
    if (layout.num_address_blocks == 0) {
        if (total_records != 0) {
            return Status::Corruption("Zero blocks with non-zero total_records");
        }
        out_entries.clear();
        return Status::OK();
    }
    if (layout.block_granularity == 0) {
        return Status::Corruption("Invalid block granularity");
    }

    out_entries.resize(total_records);

    const uint32_t tail_count = BlockRecordCount(
        layout, total_records, layout.num_address_blocks - 1);
    const bool has_separate_tail =
        tail_count < layout.block_granularity ||
        layout.last_packed_size != layout.fixed_packed_size;
    const uint32_t num_regular_blocks =
        has_separate_tail ? (layout.num_address_blocks - 1) : layout.num_address_blocks;

    const uint32_t G = layout.block_granularity;
    uint32_t out_offset = 0;

    // Decode regular blocks via multi-stream pipeline (K=8 per batch)
    for (uint32_t batch_start = 0; batch_start < num_regular_blocks;
         batch_start += kMultiStreamWidth) {
        const uint32_t batch_blocks = std::min(
            kMultiStreamWidth, num_regular_blocks - batch_start);

        DecodeMultiStream(layout, blocks.data() + batch_start,
                          batch_blocks, out_entries.data() + out_offset);
        out_offset += batch_blocks * G;
    }

    // Decode tail block separately
    if (has_separate_tail) {
        std::vector<AddressEntry> tail_entries;
        DecodeSingleBlock(layout, blocks.back(), tail_count, tail_entries);
        std::copy(tail_entries.begin(), tail_entries.end(),
                  out_entries.begin() + out_offset);
        out_offset += tail_count;
    }

    if (out_offset != total_records) {
        return Status::Corruption("Decoded address count mismatch");
    }

    return Status::OK();
}

uint32_t AddressColumn::BlockRecordCount(const AddressColumnLayout& layout,
                                         uint32_t total_records,
                                         uint32_t block_idx) {
    if (block_idx >= layout.num_address_blocks) return 0;
    if (layout.num_address_blocks == 0) return 0;
    if (block_idx + 1 < layout.num_address_blocks) {
        return layout.block_granularity;
    }

    const uint64_t regular_records =
        static_cast<uint64_t>(layout.num_address_blocks - 1) * layout.block_granularity;
    if (total_records <= regular_records) return 0;
    return total_records - static_cast<uint32_t>(regular_records);
}

uint32_t AddressColumn::BlockPackedSize(const AddressColumnLayout& layout,
                                        uint32_t block_idx) {
    if (block_idx >= layout.num_address_blocks) return 0;
    if (layout.num_address_blocks == 0) return 0;
    return (block_idx + 1 < layout.num_address_blocks)
        ? layout.fixed_packed_size
        : layout.last_packed_size;
}

}  // namespace storage
}  // namespace vdb
