#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace query {

/// Location of a cluster block within the .clu file.
struct ClusterBlockLocation {
    uint64_t offset;       // Absolute byte offset in .clu file
    uint64_t size;         // Block size in bytes
    uint32_t num_records;  // Record count in this cluster
};

/// Parsed cluster data from a raw block buffer (async-read path).
///
/// Owns the raw block buffer; codes_start is a zero-copy pointer into it.
/// decoded_addresses is separately materialized via SIMD decode.
/// Move-only.
struct ParsedCluster {
    std::unique_ptr<uint8_t[]> block_buf;  // Owns entire raw block
    const uint8_t* codes_start = nullptr;  // Zero-copy into block_buf
    uint32_t code_entry_size = 0;          // Bytes per RaBitQ code entry
    uint32_t num_records = 0;
    float epsilon = 0.0f;                  // r_max = max(‖o-c‖) within cluster
    std::vector<AddressEntry> decoded_addresses;

    ParsedCluster() = default;
    ParsedCluster(ParsedCluster&&) = default;
    ParsedCluster& operator=(ParsedCluster&&) = default;
    VDB_DISALLOW_COPY(ParsedCluster);
};

}  // namespace query
}  // namespace vdb
