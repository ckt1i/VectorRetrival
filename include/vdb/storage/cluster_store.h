#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/storage/address_column.h"

namespace vdb {
namespace storage {

// ============================================================================
// ClusterStoreWriter — builds a .clu file for one IVF cluster
// ============================================================================

/// Writes the ClusterStore file (.clu) for a single IVF cluster.
///
/// File layout:
///   ┌──────────────────────────────┐
///   │  Centroid vector (dim × 4B)  │
///   ├──────────────────────────────┤
///   │  RaBitQ codes                │
///   │   code[0]: uint64_t[nwords]  │
///   │   code[1]: uint64_t[nwords]  │
///   │   ...                        │
///   ├──────────────────────────────┤
///   │  Norms (float[N])            │
///   ├──────────────────────────────┤
///   │  Address blocks              │
///   │   block[0].packed bytes      │
///   │   block[1].packed bytes      │
///   │   ...                        │
///   └──────────────────────────────┘
///
/// After all data is written, Finalize() serializes a ClusterMeta
/// (in-memory struct) that records the offsets and lengths of each region.
///
class ClusterStoreWriter {
 public:
    ClusterStoreWriter();
    ~ClusterStoreWriter();

    VDB_DISALLOW_COPY_AND_MOVE(ClusterStoreWriter);

    /// Open a new .clu file for writing.
    ///
    /// @param path          Output file path
    /// @param cluster_id    Cluster ID
    /// @param dim           Vector dimensionality
    /// @param rabitq_config RaBitQ configuration
    /// @return Status
    Status Open(const std::string& path,
                uint32_t cluster_id,
                Dim dim,
                const RaBitQConfig& rabitq_config);

    /// Write the centroid vector. Must be called exactly once before
    /// WriteVectors().
    ///
    /// @param centroid  Centroid vector (dim floats)
    /// @return Status
    Status WriteCentroid(const float* centroid);

    /// Write all RaBitQ encoded vectors and their norms.
    ///
    /// @param codes  Encoded vectors from RaBitQEncoder
    /// @return Status
    Status WriteVectors(const std::vector<rabitq::RaBitQCode>& codes);

    /// Write address blocks (from AddressColumn::Encode).
    ///
    /// @param blocks  Encoded address blocks
    /// @return Status
    Status WriteAddressBlocks(const std::vector<AddressBlock>& blocks);

    /// Finalize and close the file.
    ///
    /// @param data_file_path  Relative path to the companion .dat file
    /// @return Status
    Status Finalize(const std::string& data_file_path);

    /// Get the cluster metadata after finalization.
    /// Contains offsets/lengths for all regions.
    struct ClusterInfo {
        uint32_t cluster_id;
        uint32_t num_records;
        Dim dim;
        RaBitQConfig rabitq_config;
        std::string data_file_path;

        uint64_t centroid_offset;
        uint32_t centroid_length;

        uint64_t rabitq_data_offset;
        uint32_t rabitq_data_length;

        uint64_t norms_offset;
        uint32_t norms_length;

        // Address blocks metadata
        std::vector<AddressBlock> address_blocks;
    };

    const ClusterInfo& info() const { return info_; }

 private:
    std::ofstream file_;
    std::string path_;
    ClusterInfo info_;
    uint64_t current_offset_ = 0;
    bool centroid_written_ = false;
    bool vectors_written_ = false;
    bool address_written_ = false;
    bool finalized_ = false;
};

// ============================================================================
// ClusterStoreReader — reads a .clu file
// ============================================================================

/// Reads a ClusterStore file (.clu) and provides access to:
///   - Centroid vector
///   - RaBitQ encoded vectors (codes + norms)
///   - Address column (for record → DataFile offset lookup)
///
class ClusterStoreReader {
 public:
    ClusterStoreReader();
    ~ClusterStoreReader();

    VDB_DISALLOW_COPY(ClusterStoreReader);
    ClusterStoreReader(ClusterStoreReader&& other) noexcept;
    ClusterStoreReader& operator=(ClusterStoreReader&& other) noexcept;

    /// Open a .clu file using the metadata from the writer.
    ///
    /// @param path  File path
    /// @param info  ClusterInfo from ClusterStoreWriter (provides offsets)
    /// @return Status
    Status Open(const std::string& path,
                const ClusterStoreWriter::ClusterInfo& info);

    /// Read ClusterInfo from a .clu file's embedded trailer.
    /// This allows opening a .clu file without having the original
    /// ClusterInfo from the writer — the metadata is reconstructed from
    /// the trailer that Finalize() appends at the end of the file.
    ///
    /// @param path  File path to the .clu file
    /// @param out   Output ClusterInfo
    /// @return Status
    static Status ReadInfo(const std::string& path,
                           ClusterStoreWriter::ClusterInfo* out);

    /// Close the file.
    void Close();

    /// Load the centroid vector.
    /// @param out  Output vector (will be resized to dim)
    Status LoadCentroid(std::vector<float>& out) const;

    /// Load a single RaBitQ code.
    /// @param record_idx  Record index [0, num_records)
    /// @param out_code    Output code words
    /// @param out_norm    Output norm value
    Status LoadCode(uint32_t record_idx,
                    std::vector<uint64_t>& out_code,
                    float& out_norm) const;

    /// Load multiple RaBitQ codes.
    /// @param indices   Record indices to load
    /// @param out_codes Output RaBitQCode structs
    Status LoadCodes(const std::vector<uint32_t>& indices,
                     std::vector<rabitq::RaBitQCode>& out_codes) const;

    /// Load all norms at once.
    /// @param out  Output norms array (will be resized to num_records)
    Status LoadAllNorms(std::vector<float>& out) const;

    /// Get address entry for a record (decoded from address blocks).
    /// @param record_idx  Record index
    /// @return AddressEntry
    AddressEntry GetAddress(uint32_t record_idx) const;

    /// Get address entries for multiple records.
    std::vector<AddressEntry> GetAddresses(
        const std::vector<uint32_t>& indices) const;

    /// Accessors
    bool is_open() const { return fd_ >= 0; }
    uint32_t cluster_id() const { return info_.cluster_id; }
    uint32_t num_records() const { return info_.num_records; }
    Dim dim() const { return info_.dim; }
    const RaBitQConfig& rabitq_config() const { return info_.rabitq_config; }
    const std::string& data_file_path() const { return info_.data_file_path; }

 private:
    int fd_ = -1;
    ClusterStoreWriter::ClusterInfo info_;

    /// Number of uint64_t words per code
    uint32_t num_code_words() const { return (info_.dim + 63) / 64; }

    /// Byte size of one code entry (code words + norm float)
    uint32_t code_entry_size() const {
        return num_code_words() * sizeof(uint64_t) + sizeof(float);
    }
};

}  // namespace storage
}  // namespace vdb
