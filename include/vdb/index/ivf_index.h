#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/storage/segment.h"

namespace vdb {
namespace index {

// ============================================================================
// IvfIndex — IVF index for query-time cluster selection
// ============================================================================

/// IVF index that loads centroids and segment metadata from disk.
///
/// Provides:
///   1. Find the `nprobe` nearest clusters for a given query.
///   2. Access to the underlying Segment (ClusterStoreReader / DataFileReader).
///   3. Access to the ConANN classifier.
///   4. Access to the shared RotationMatrix (loaded from rotation.bin).
///
/// On-disk layout (loaded by Open):
///   <dir>/
///   ├── segment.meta         # SegmentMeta FlatBuffers
///   ├── centroids.bin        # nlist × dim × float32 (raw binary)
///   ├── rotation.bin         # dim × dim × float32 (raw binary)
///   ├── cluster.clu          # unified ClusterStore file
///   └── data.dat             # unified DataFile
///
class IvfIndex {
 public:
    IvfIndex();
    ~IvfIndex();

    VDB_DISALLOW_COPY_AND_MOVE(IvfIndex);

    /// Open an IVF index from a directory.
    ///
    /// Reads segment.meta, centroids.bin, rotation.bin, and registers all
    /// clusters into the internal Segment.
    ///
    /// @param dir  Path to the directory containing the index files
    /// @return     Status
    Status Open(const std::string& dir, bool use_direct_io = false);

    /// Find the nprobe nearest clusters for a query vector.
    ///
    /// Computes L2 squared distance from the query to all centroids,
    /// then returns the nprobe clusters with the smallest distances.
    ///
    /// @param query   Raw query vector (length = dim)
    /// @param nprobe  Number of clusters to probe
    /// @return        ClusterIDs of the nprobe nearest clusters,
    ///                ordered by distance (nearest first)
    std::vector<ClusterID> FindNearestClusters(const float* query,
                                                uint32_t nprobe) const;

    /// Get the ConANN classifier.
    const ConANN& conann() const { return conann_; }

    /// Get the underlying Segment (mutable for lazy-opening readers).
    storage::Segment& segment() { return segment_; }

    /// Get the shared rotation matrix.
    const rabitq::RotationMatrix& rotation() const { return *rotation_; }

    /// Number of clusters (nlist).
    uint32_t nlist() const { return nlist_; }

    /// Vector dimensionality.
    Dim dim() const { return dim_; }

    /// Get centroid for a specific cluster (row-major offset into centroids_).
    const float* centroid(uint32_t cluster_idx) const {
        return centroids_.data() + static_cast<size_t>(cluster_idx) * dim_;
    }

    /// Get all cluster IDs.
    const std::vector<ClusterID>& cluster_ids() const { return cluster_ids_; }

    /// The directory this index was loaded from.
    const std::string& dir() const { return dir_; }

    /// Payload column schemas (loaded from segment.meta).
    const std::vector<ColumnSchema>& payload_schemas() const { return payload_schemas_; }

 private:
    std::string dir_;
    Dim dim_ = 0;
    uint32_t nlist_ = 0;

    std::vector<float> centroids_;         // row-major, nlist × dim
    std::vector<ClusterID> cluster_ids_;   // ordered cluster IDs
    ConANN conann_{0.0f, 0.0f};            // default, overwritten by Open
    storage::Segment segment_;
    std::unique_ptr<rabitq::RotationMatrix> rotation_;

    // Payload schemas (loaded from segment meta, for DataFileReader)
    std::vector<ColumnSchema> payload_schemas_;

#ifdef VDB_USE_MKL
    // Precomputed ||c||² for each centroid (MKL-accelerated distance)
    std::vector<float> centroid_norms_;
#endif
};

}  // namespace index
}  // namespace vdb
