#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"

namespace vdb {
namespace storage {

// ============================================================================
// AddressBlock — encoded address info for up to 64 records
// ============================================================================

/// A single address block encoding (offset, size) pairs for up to
/// `block_granularity` records (default 64).
///
/// Layout in a .clu file:
///   base_offset  : uint64   — first record's DataFile byte offset
///   bit_width    : uint8    — bits per size value (1..32)
///   packed_sizes : ubyte[]  — bit-packed size values (LSB-first)
///
/// Decoding flow (SIMD-accelerated):
///   1. bit_unpack(packed_sizes, bit_width) → sizes[64]
///   2. exclusive_prefix_sum(sizes[])       → offsets[64]
///   3. entry[i] = { base_offset + offsets[i], sizes[i] }
///
struct AddressBlock {
    uint64_t base_offset;            // First record's offset in DataFile
    uint8_t  bit_width;              // Bits per size value
    uint32_t record_count;           // Actual records in this block (≤ 64)
    std::vector<uint8_t> packed;     // Bit-packed sizes data
};

// ============================================================================
// AddressColumn — encode / decode address blocks
// ============================================================================

/// Encodes and decodes the Address Column used by ClusterStore.
///
/// The Address Column maps each record index to its physical location
/// (offset, size) in the corresponding DataFile. Records are grouped into
/// blocks of `block_granularity` (default 64), aligned with RaBitQ's
/// block processing.
///
/// **Encoding** (offline, index construction):
///   Given N AddressEntry values, produce ⌈N/64⌉ AddressBlocks.
///   Each block stores:
///     - base_offset = first record's file offset
///     - bit_width   = minimum bits to represent max(sizes) in the block
///     - packed[]    = bit-packed sizes using BitpackCodec
///
/// **Decoding** (query hot path):
///   Given a block, use SIMD bit_unpack + prefix_sum to reconstruct all
///   64 (offset, size) pairs in one shot.
///
class AddressColumn {
 public:
    /// Default block granularity (records per block).
    static constexpr uint32_t kDefaultBlockGranularity = 64;

    // ---- Encoding (construction time) ------------------------------------

    /// Encode a list of AddressEntry values into blocks.
    ///
    /// @param entries           Array of (offset, size) pairs, sorted by offset
    /// @param block_granularity Records per block (default 64)
    /// @return                  Encoded address blocks
    static std::vector<AddressBlock> Encode(
        const std::vector<AddressEntry>& entries,
        uint32_t block_granularity = kDefaultBlockGranularity);

    // ---- Decoding (query time) -------------------------------------------

    /// Decode a single block into AddressEntry values.
    ///
    /// Uses SIMD bit_unpack + prefix_sum for the hot path.
    ///
    /// @param block        Encoded block to decode
    /// @param out_entries  Output vector (will be resized to block.record_count)
    static void DecodeBlock(const AddressBlock& block,
                            std::vector<AddressEntry>& out_entries);

    /// Look up a single record's address from a block array.
    ///
    /// @param blocks       All blocks for this cluster
    /// @param record_idx   Global record index [0, N)
    /// @param granularity  Records per block (must match Encode)
    /// @return             The record's (offset, size)
    static AddressEntry Lookup(const std::vector<AddressBlock>& blocks,
                               uint32_t record_idx,
                               uint32_t granularity = kDefaultBlockGranularity);

    /// Batch lookup: decode multiple record indices.
    ///
    /// @param blocks       All blocks for this cluster
    /// @param indices      Record indices to look up
    /// @param granularity  Records per block
    /// @return             Corresponding AddressEntry values (same order)
    static std::vector<AddressEntry> BatchLookup(
        const std::vector<AddressBlock>& blocks,
        const std::vector<uint32_t>& indices,
        uint32_t granularity = kDefaultBlockGranularity);

    /// Total number of records across all blocks.
    static uint32_t TotalRecords(const std::vector<AddressBlock>& blocks);

 private:
    /// Encode a single block of up to `count` entries.
    static AddressBlock EncodeSingleBlock(const AddressEntry* entries,
                                          uint32_t count);
};

}  // namespace storage
}  // namespace vdb
