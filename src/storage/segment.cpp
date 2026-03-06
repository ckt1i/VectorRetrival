#include "vdb/storage/segment.h"

namespace vdb {
namespace storage {

// ============================================================================
// Construction / Destruction
// ============================================================================

Segment::Segment() = default;

Segment::~Segment() = default;

// ============================================================================
// AddCluster
// ============================================================================

Status Segment::AddCluster(const ClusterStoreWriter::ClusterInfo& info,
                            const std::string& clu_path,
                            const std::string& dat_path,
                            Dim dim,
                            const std::vector<ColumnSchema>& payload_schemas) {
    uint32_t cid = info.cluster_id;
    if (cluster_entries_.count(cid)) {
        return Status::AlreadyExists(
            "Cluster " + std::to_string(cid) + " already registered");
    }

    ClusterEntry entry;
    entry.info = info;
    entry.clu_path = clu_path;
    entry.dat_path = dat_path;
    entry.dim = dim;
    entry.payload_schemas = payload_schemas;

    cluster_entries_[cid] = std::move(entry);
    return Status::OK();
}

// ============================================================================
// GetCluster
// ============================================================================

std::shared_ptr<ClusterStoreReader> Segment::GetCluster(uint32_t cluster_id) {
    // Check cache
    auto it = clu_cache_.find(cluster_id);
    if (it != clu_cache_.end()) {
        return it->second;
    }

    // Find entry
    auto eit = cluster_entries_.find(cluster_id);
    if (eit == cluster_entries_.end()) {
        return nullptr;
    }

    // Open reader
    auto reader = std::make_shared<ClusterStoreReader>();
    Status s = reader->Open(eit->second.clu_path, eit->second.info);
    if (!s.ok()) {
        return nullptr;
    }

    clu_cache_[cluster_id] = reader;
    return reader;
}

// ============================================================================
// GetDataFile
// ============================================================================

std::shared_ptr<DataFileReader> Segment::GetDataFile(uint32_t cluster_id) {
    // Check cache
    auto it = dat_cache_.find(cluster_id);
    if (it != dat_cache_.end()) {
        return it->second;
    }

    // Find entry
    auto eit = cluster_entries_.find(cluster_id);
    if (eit == cluster_entries_.end()) {
        return nullptr;
    }

    // Open reader
    auto reader = std::make_shared<DataFileReader>();
    Status s = reader->Open(eit->second.dat_path,
                            eit->second.dim,
                            eit->second.payload_schemas);
    if (!s.ok()) {
        return nullptr;
    }

    dat_cache_[cluster_id] = reader;
    return reader;
}

// ============================================================================
// Utility
// ============================================================================

std::vector<uint32_t> Segment::cluster_ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(cluster_entries_.size());
    for (const auto& kv : cluster_entries_) {
        ids.push_back(kv.first);
    }
    return ids;
}

uint64_t Segment::total_records() const {
    uint64_t total = 0;
    for (const auto& kv : cluster_entries_) {
        total += kv.second.info.num_records;
    }
    return total;
}

}  // namespace storage
}  // namespace vdb
