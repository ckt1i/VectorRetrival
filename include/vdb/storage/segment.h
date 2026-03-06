#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/data_file_reader.h"

namespace vdb {
namespace storage {

// ============================================================================
// Segment — manages all clusters for one IVF segment
// ============================================================================

/// A Segment groups all clusters belonging to one IVF index.
///
/// On disk:
///   segment_<id>/
///   ├── cluster_0000.clu
///   ├── cluster_0000.dat
///   ├── cluster_0001.clu
///   ├── cluster_0001.dat
///   └── ...
///
/// The Segment holds metadata for every cluster and provides access to
/// ClusterStoreReader / DataFileReader instances.
///
class Segment {
 public:
    Segment();
    ~Segment();

    VDB_DISALLOW_COPY_AND_MOVE(Segment);

    /// Register a cluster (after building with Writer).
    ///
    /// @param info            ClusterInfo from the writer
    /// @param clu_path        Absolute path to the .clu file
    /// @param dat_path        Absolute path to the .dat file
    /// @param dim             Vector dimensionality
    /// @param payload_schemas Payload schemas for DataFileReader
    /// @return Status
    Status AddCluster(const ClusterStoreWriter::ClusterInfo& info,
                      const std::string& clu_path,
                      const std::string& dat_path,
                      Dim dim,
                      const std::vector<ColumnSchema>& payload_schemas = {});

    /// Get a ClusterStoreReader for a given cluster.
    ///
    /// Opens the reader on first call, caches for subsequent calls.
    /// @param cluster_id  Cluster ID
    /// @return shared_ptr to reader, or nullptr if not found
    std::shared_ptr<ClusterStoreReader> GetCluster(uint32_t cluster_id);

    /// Get a DataFileReader for a given cluster.
    ///
    /// Opens the reader on first call, caches for subsequent calls.
    /// @param cluster_id  Cluster ID
    /// @return shared_ptr to reader, or nullptr if not found
    std::shared_ptr<DataFileReader> GetDataFile(uint32_t cluster_id);

    /// Number of registered clusters.
    uint32_t num_clusters() const {
        return static_cast<uint32_t>(cluster_entries_.size());
    }

    /// Get all cluster IDs.
    std::vector<uint32_t> cluster_ids() const;

    /// Total records across all clusters.
    uint64_t total_records() const;

 private:
    /// Per-cluster registration data
    struct ClusterEntry {
        ClusterStoreWriter::ClusterInfo info;
        std::string clu_path;
        std::string dat_path;
        Dim dim;
        std::vector<ColumnSchema> payload_schemas;
    };

    std::map<uint32_t, ClusterEntry> cluster_entries_;

    // Caches (opened on demand)
    std::map<uint32_t, std::shared_ptr<ClusterStoreReader>> clu_cache_;
    std::map<uint32_t, std::shared_ptr<DataFileReader>> dat_cache_;
};

}  // namespace storage
}  // namespace vdb
