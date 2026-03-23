#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/storage/address_column.h"

namespace vdb {
namespace storage {

// ============================================================================
// ClusterStoreWriter — builds a unified .clu file for ALL IVF clusters
// ============================================================================

/// Writes a unified ClusterStore file (.clu) containing all IVF clusters.
///
/// File layout:
///   ┌────────────────────────────────────────────────────┐
///   │  Global Header                                     │
///   │    magic, version, num_clusters, dim, rabitq_cfg   │
///   │    data_file_path                                  │
///   ├────────────────────────────────────────────────────┤
///   │  Cluster Lookup Table (num_clusters entries)        │
///   │    Entry[i]:                                        │
///   │      cluster_id, num_records,                      │
///   │      centroid[dim],                                │
///   │      block_offset, block_size                      │
///   ├────────────────────────────────────────────────────┤
///   │  Per-Cluster Data Blocks (in lookup table order)   │
///   │    Block[i]:                                        │
///   │      RaBitQ codes (uint64[nwords] × num_records)   │
///   │      Address packed data                           │
///   │      Block mini-trailer                            │
///   └────────────────────────────────────────────────────┘
///
/// Usage:
///   writer.Open(path, num_clusters, dim, rabitq_config)
///   for each cluster:
///     writer.BeginCluster(id, num_records, centroid)
///     writer.WriteVectors(codes)
///     writer.WriteAddressBlocks(address_column)
///     writer.EndCluster()
///   writer.Finalize(data_file_path)
///
class ClusterStoreWriter {
 public:
    ClusterStoreWriter();
    ~ClusterStoreWriter();

    VDB_DISALLOW_COPY_AND_MOVE(ClusterStoreWriter);

    /// Per-cluster metadata stored in the lookup table.
    struct ClusterLookupEntry {
        uint32_t cluster_id;
        uint32_t num_records;
        float epsilon = 0.0f;          // per-cluster RaBitQ reconstruction error (P95)
        std::vector<float> centroid;   // dim floats
        uint64_t block_offset;         // absolute byte offset in .clu
        uint64_t block_size;           // byte length of block
    };

    /// Global metadata for the .clu file.
    struct GlobalInfo {
        uint32_t num_clusters;
        Dim dim;
        RaBitQConfig rabitq_config;
        std::string data_file_path;

        std::vector<ClusterLookupEntry> lookup_table;
    };

    /// Open a new .clu file for writing.
    ///
    /// Writes the global header and a zero-filled lookup table.
    ///
    /// @param path          Output file path
    /// @param num_clusters  Total number of clusters to write
    /// @param dim           Vector dimensionality
    /// @param rabitq_config RaBitQ configuration
    /// @return Status
    Status Open(const std::string& path,
                uint32_t num_clusters,
                Dim dim,
                const RaBitQConfig& rabitq_config);

    /// Begin writing a cluster's data block.
    ///
    /// @param cluster_id   Cluster ID
    /// @param num_records  Number of records in this cluster
    /// @param centroid     Centroid vector (dim floats)
    /// @return Status
    Status BeginCluster(uint32_t cluster_id,
                        uint32_t num_records,
                        const float* centroid,
                        float epsilon = 0.0f);

    /// Write RaBitQ encoded vectors for the current cluster.
    ///
    /// @param codes  Encoded vectors from RaBitQEncoder
    /// @return Status
    Status WriteVectors(const std::vector<rabitq::RaBitQCode>& codes);

    /// Write address blocks for the current cluster.
    ///
    /// @param column  Encoded address column
    /// @return Status
    Status WriteAddressBlocks(const EncodedAddressColumn& column);

    /// End the current cluster's data block.
    /// Writes the block mini-trailer and patches the lookup table entry.
    ///
    /// @return Status
    Status EndCluster();

    /// Finalize and close the file.
    /// Patches the data_file_path in the global header.
    ///
    /// @param data_file_path  Relative path to the companion .dat file
    /// @return Status
    Status Finalize(const std::string& data_file_path);

    /// Get the global metadata after finalization.
    const GlobalInfo& info() const { return info_; }

 private:
    std::fstream file_;
    std::string path_;
    GlobalInfo info_;
    uint64_t current_offset_ = 0;

    // Lookup table file positions for patching
    uint64_t lookup_table_start_ = 0;
    uint64_t header_data_file_path_offset_ = 0;

    // Per-cluster write state
    uint32_t current_cluster_index_ = 0;
    uint64_t block_start_ = 0;
    bool in_cluster_ = false;
    bool vectors_written_ = false;
    bool address_written_ = false;
    bool finalized_ = false;

    // Current cluster's encoded address column (for mini-trailer)
    EncodedAddressColumn current_address_column_;

    /// Size in bytes of one lookup table entry on disk.
    uint64_t lookup_entry_size() const;
};

// ============================================================================
// ClusterStoreReader — reads a unified .clu file
// ============================================================================

/// Reads a unified ClusterStore file (.clu) containing all IVF clusters.
///
/// On Open():
///   - Reads the global header
///   - Reads the entire cluster lookup table into memory
///     (centroids + block offsets/sizes)
///
/// Per-cluster data (codes, addresses) is loaded lazily via
/// EnsureClusterLoaded().
///
class ClusterStoreReader {
 public:
    ClusterStoreReader();
    ~ClusterStoreReader();

    VDB_DISALLOW_COPY(ClusterStoreReader);
    ClusterStoreReader(ClusterStoreReader&& other) noexcept;
    ClusterStoreReader& operator=(ClusterStoreReader&& other) noexcept;

    /// Open a .clu file.
    /// Reads global header and the full lookup table into memory.
    ///
    /// @param path  File path
    /// @return Status
    Status Open(const std::string& path);

    /// Close the file.
    void Close();

    // ---- Lookup table accessors (all in-memory, O(1)) -------------------

    /// Number of clusters in the file.
    uint32_t num_clusters() const { return info_.num_clusters; }

    /// Vector dimensionality.
    Dim dim() const { return info_.dim; }

    /// RaBitQ configuration.
    const RaBitQConfig& rabitq_config() const { return info_.rabitq_config; }

    /// Data file path.
    const std::string& data_file_path() const { return info_.data_file_path; }

    /// Get all cluster IDs (in file order).
    std::vector<uint32_t> cluster_ids() const;

    /// Number of records in a specific cluster.
    uint32_t GetNumRecords(uint32_t cluster_id) const;

    /// Get the centroid vector for a cluster (pointer into in-memory table).
    /// Returns nullptr if cluster_id not found.
    const float* GetCentroid(uint32_t cluster_id) const;

    /// Get the per-cluster epsilon (RaBitQ reconstruction error P95).
    /// Returns 0.0f if cluster_id not found.
    float GetEpsilon(uint32_t cluster_id) const;

    /// Total records across all clusters.
    uint64_t total_records() const;

    // ---- Per-cluster lazy loading ----------------------------------------

    /// Ensure a cluster's per-record data is loaded and decoded.
    /// First call: pread the cluster's data block, parse mini-trailer,
    ///             load address packed bytes, SIMD-decode all addresses.
    /// Subsequent calls: no-op.
    ///
    /// @param cluster_id  Cluster ID to load
    /// @return Status
    Status EnsureClusterLoaded(uint32_t cluster_id);

    /// Get address entry for a record in a cluster.
    /// Requires EnsureClusterLoaded() to have been called.
    AddressEntry GetAddress(uint32_t cluster_id,
                            uint32_t record_idx) const;

    /// Get address entries for multiple records in a cluster.
    std::vector<AddressEntry> GetAddresses(
        uint32_t cluster_id,
        const std::vector<uint32_t>& indices) const;

    /// Load a single RaBitQ code from a cluster.
    Status LoadCode(uint32_t cluster_id,
                    uint32_t record_idx,
                    std::vector<uint64_t>& out_code) const;

    /// Load multiple RaBitQ codes from a cluster.
    Status LoadCodes(uint32_t cluster_id,
                     const std::vector<uint32_t>& indices,
                     std::vector<rabitq::RaBitQCode>& out_codes) const;

    /// Get raw pointer to a code entry in the cached codes_buffer.
    /// Returns nullptr if cluster not loaded or record_idx out of range.
    /// Requires EnsureClusterLoaded() to have been called.
    const uint8_t* GetCodePtr(uint32_t cluster_id,
                              uint32_t record_idx) const;

    // ---- Async query support (Phase 8) ------------------------------------

    /// File descriptor for the .clu file (for io_uring direct submission).
    int clu_fd() const { return fd_; }

    /// Look up a cluster's block location in the lookup table (pure memory).
    /// Returns nullopt if cluster_id not found.
    std::optional<query::ClusterBlockLocation> GetBlockLocation(
        uint32_t cluster_id) const;

    /// Parse a raw block buffer (already read from disk) into a ParsedCluster.
    /// Pure CPU operation — no I/O.  block_buf ownership transfers to out.
    Status ParseClusterBlock(uint32_t cluster_id,
                              std::unique_ptr<uint8_t[]> block_buf,
                              uint64_t block_size,
                              query::ParsedCluster& out);

    /// Whether the file is open.
    bool is_open() const { return fd_ >= 0; }

 private:
    int fd_ = -1;

    /// Parsed global header + lookup table
    ClusterStoreWriter::GlobalInfo info_;

    /// Map cluster_id → index into info_.lookup_table
    std::map<uint32_t, uint32_t> cluster_index_;

    /// Per-cluster loaded data (populated lazily by EnsureClusterLoaded)
    struct ClusterData {
        // Code region offset (absolute in .clu file)
        uint64_t codes_offset = 0;
        uint32_t codes_length = 0;

        // Full codes region cached in memory (loaded by EnsureClusterLoaded)
        std::vector<uint8_t> codes_buffer;

        // Address layout, block payloads, and decoded addresses
        AddressColumnLayout address_layout;
        std::vector<AddressBlock> address_blocks;
        std::vector<AddressEntry> decoded_addresses;
    };

    std::map<uint32_t, ClusterData> loaded_clusters_;

    /// Number of uint64_t words per code
    uint32_t num_code_words() const { return (info_.dim + 63) / 64; }

    /// Byte size of one code entry (v5: code_bits + norm(f32) + sum_x(u32))
    uint32_t code_entry_size() const {
        return num_code_words() * sizeof(uint64_t) + sizeof(float) + sizeof(uint32_t);
    }
};

}  // namespace storage
}  // namespace vdb
