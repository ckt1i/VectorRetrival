#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"

namespace vdb {
namespace storage {

// ============================================================================
// AddressColumnLayout / AddressBlock / EncodedAddressColumn
// ============================================================================

/// Cluster-scoped address layout shared by all address blocks in one cluster.
struct AddressColumnLayout {
    uint32_t page_size = kDefaultPageSize;   // Byte granularity
    uint8_t bit_width = 1;                   // Bits per page-unit size value
    uint32_t block_granularity = 0;          // Record count for regular blocks
    uint32_t fixed_packed_size = 0;          // Bytes for regular block payloads
    uint32_t last_packed_size = 0;           // Bytes for tail block payload
    uint32_t num_address_blocks = 0;         // Number of address blocks
};

/// A single encoded address block.
///
/// All offsets and sizes are stored in **page units** (not bytes). The cluster
///-shared decode parameters live in `AddressColumnLayout`; each block only
/// stores the fields that vary per block.
///
/// Layout in a .clu file (v4):
///   base_offset  : uint32   — first record's page index (byte_offset / page_size)
///   packed_sizes : ubyte[]  — bit-packed page-unit size values (LSB-first)
///
/// uint32 page index is sufficient for 16 TB addressing at page_size=4096.
struct AddressBlock {
    uint32_t base_offset = 0;        // First record's page index
    std::vector<uint8_t> packed;     // Bit-packed page-unit sizes
};

/// Full encoded address column for one cluster.
struct EncodedAddressColumn {
    AddressColumnLayout layout;
    std::vector<AddressBlock> blocks;
    uint32_t total_records = 0;
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
    /// Default fixed packed payload size for regular address blocks.
    static constexpr uint32_t kDefaultFixedPackedSize = 64;

    /// Multi-stream SIMD width: number of blocks decoded in parallel.
    /// Matches AVX2 lane count (8 × uint32_t per __m256i).
    static constexpr uint32_t kMultiStreamWidth = 8;

    /// Default SIMD decode batch width in blocks (legacy, now kMultiStreamWidth).
    static constexpr uint32_t kDefaultDecodeBatchBlocks = kMultiStreamWidth;

    // ---- Encoding (construction time) ------------------------------------

    /// Encode a list of AddressEntry values into a cluster-scoped address
    /// column.
    ///
    /// Byte-level offsets and sizes are converted to **page units** before
    /// bit-packing. One cluster-shared `bit_width` is chosen from the maximum
    /// page-unit size in the cluster. Regular blocks are padded to
    /// `fixed_packed_size` bytes so multiple blocks can be decoded in lockstep;
    /// only the final block may use a shorter packed payload.
    ///
    /// @param entries           Array of (offset, size) pairs, sorted by offset
    /// @param fixed_packed_size Target byte size for regular packed blocks
    /// @param page_size         Page granularity in bytes (default 4096).
    ///                          Use 1 for byte-level addressing (no alignment).
    /// @return                  Encoded address column
    static EncodedAddressColumn Encode(
        const std::vector<AddressEntry>& entries,
        uint32_t fixed_packed_size = kDefaultFixedPackedSize,
        uint32_t page_size = kDefaultPageSize);

    // ---- Decoding (query time) -------------------------------------------

    /// Decode a single block into AddressEntry values.
    ///
    /// Uses SIMD bit_unpack + prefix_sum for the hot path.
    ///
    /// @param layout       Cluster-shared address layout
    /// @param block        Encoded block to decode
    /// @param record_count Logical records in the block
    /// @param out_entries  Output vector (will be resized to record_count)
    static void DecodeSingleBlock(const AddressColumnLayout& layout,
                                  const AddressBlock& block,
                                  uint32_t record_count,
                                  std::vector<AddressEntry>& out_entries);

    /// Decode all blocks in one encoded address column.
    static Status Decode(const EncodedAddressColumn& column,
                         std::vector<AddressEntry>& out_entries);

    /// Decode multiple regular blocks in batches and the tail block separately.
    /// Uses multi-stream SIMD pipeline (K=8 blocks per batch) for regular
    /// blocks, and DecodeSingleBlock for the tail block.
    static Status DecodeBatchBlocks(const AddressColumnLayout& layout,
                                    const std::vector<AddressBlock>& blocks,
                                    uint32_t total_records,
                                    std::vector<AddressEntry>& out_entries);

    /// Decode up to kMultiStreamWidth regular blocks using the multi-stream
    /// SIMD pipeline: BitUnpack → Transpose → PrefixSumMulti → Transpose
    /// → Materialize.
    ///
    /// @param layout        Cluster-shared layout
    /// @param blocks        Pointer to first block in the batch
    /// @param num_blocks    Number of blocks in this batch (1..kMultiStreamWidth)
    /// @param out_entries   Output pointer (must have room for num_blocks × G entries)
    static void DecodeMultiStream(const AddressColumnLayout& layout,
                                   const AddressBlock* blocks,
                                   uint32_t num_blocks,
                                   AddressEntry* out_entries);

    /// Logical record count for a given block index.
    static uint32_t BlockRecordCount(const AddressColumnLayout& layout,
                                     uint32_t total_records,
                                     uint32_t block_idx);

    /// Packed byte size for a given block index.
    static uint32_t BlockPackedSize(const AddressColumnLayout& layout,
                                    uint32_t block_idx);

    /// Total number of records across all blocks.
    static uint32_t TotalRecords(const EncodedAddressColumn& column) {
        return column.total_records;
    }

 private:
    /// Encode a single block of up to `count` entries using the shared bit width.
    static AddressBlock EncodeSingleBlock(const AddressEntry* entries,
                                          uint32_t count,
                                          uint32_t page_size,
                                          uint8_t bit_width,
                                          uint32_t padded_size);
};

}  // namespace storage
}  // namespace vdb
