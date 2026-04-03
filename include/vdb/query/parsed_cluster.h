#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace query {

/// Custom deleter for buffers allocated with aligned_alloc / std::aligned_alloc.
struct FreeDeleter {
    void operator()(uint8_t* p) const noexcept { std::free(p); }
};
using AlignedBufPtr = std::unique_ptr<uint8_t[], FreeDeleter>;

/// Location of a cluster block within the .clu file.
struct ClusterBlockLocation {
    uint64_t offset;       // Absolute byte offset in .clu file
    uint64_t size;         // Block size in bytes
    uint32_t num_records;  // Record count in this cluster
};

/// Parsed cluster data from a raw block buffer (async-read path).
///
/// v7 dual-region layout:
///   Region 1: FastScan blocks (packed sign bits + norm_oc factors)
///   Region 2: ExRaBitQ entries (ex_code + ex_sign + xipnorm, only when bits > 1)
///   Region 3: Address packed data + mini-trailer
///
/// Owns the raw block buffer; pointers are zero-copy into it.
/// decoded_addresses is separately materialized via SIMD decode.
/// Move-only.
struct ParsedCluster {
    AlignedBufPtr block_buf;  // Owns entire raw block (aligned_alloc, freed via free())

    // --- Region 1: FastScan ---
    const uint8_t* fastscan_blocks = nullptr;  // Pointer to first block
    uint32_t fastscan_block_size = 0;          // Bytes per block (packed_codes + norms)
    uint32_t num_fastscan_blocks = 0;          // ceil(num_records / 32)

    // --- Region 2: ExRaBitQ ---
    const uint8_t* exrabitq_entries = nullptr;  // Pointer to first entry (nullptr if bits=1)
    uint32_t exrabitq_entry_size = 0;           // 2*D + 4 (0 if bits=1)

    // --- Existing ---
    uint32_t num_records = 0;
    float epsilon = 0.0f;                  // r_max = max(‖o-c‖) within cluster
    std::vector<AddressEntry> decoded_addresses;

    // --- Legacy (kept for compatibility during transition) ---
    const uint8_t* codes_start = nullptr;  // Zero-copy into block_buf
    uint32_t code_entry_size = 0;          // Bytes per RaBitQ code entry

    ParsedCluster() = default;
    ParsedCluster(ParsedCluster&&) = default;
    ParsedCluster& operator=(ParsedCluster&&) = default;
    VDB_DISALLOW_COPY(ParsedCluster);

    // --- Helpers ---
    /// Get norm_oc for vector vec_in_block within FastScan block block_idx.
    float norm_oc(uint32_t block_idx, uint32_t vec_in_block) const {
        const uint8_t* block = fastscan_blocks + block_idx * fastscan_block_size;
        uint32_t packed_codes_size = fastscan_block_size - 32 * sizeof(float);
        const float* norms = reinterpret_cast<const float*>(block + packed_codes_size);
        return norms[vec_in_block];
    }

    /// Get ex_code pointer for global vector idx (Region 2).
    const uint8_t* ex_code(uint32_t vec_idx) const {
        return exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size;
    }

    /// Get ex_sign pointer for global vector idx (Region 2).
    const uint8_t* ex_sign(uint32_t vec_idx, Dim dim) const {
        return exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size + dim;
    }

    /// Get xipnorm for global vector idx (Region 2).
    float xipnorm(uint32_t vec_idx, Dim dim) const {
        float val;
        std::memcpy(&val,
                     exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size + 2 * dim,
                     sizeof(float));
        return val;
    }
};

}  // namespace query
}  // namespace vdb
