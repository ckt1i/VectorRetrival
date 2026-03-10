#include "vdb/index/ivf_index.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>

#include "vdb/simd/distance_l2.h"
#include "vdb/storage/cluster_store.h"

// FlatBuffers generated header
#include "segment_meta_generated.h"

namespace vdb {
namespace index {

// ============================================================================
// Construction / Destruction
// ============================================================================

IvfIndex::IvfIndex() = default;
IvfIndex::~IvfIndex() = default;

// ============================================================================
// Open — load index from directory
// ============================================================================

Status IvfIndex::Open(const std::string& dir) {
    dir_ = dir;

    // --- 1. Read segment.meta (FlatBuffers) ---
    const std::string meta_path = dir + "/segment.meta";
    std::ifstream meta_file(meta_path, std::ios::binary | std::ios::ate);
    if (!meta_file.is_open()) {
        return Status::IOError("Failed to open segment.meta: " + meta_path);
    }
    const auto meta_size = meta_file.tellg();
    meta_file.seekg(0);
    std::vector<uint8_t> meta_buf(static_cast<size_t>(meta_size));
    meta_file.read(reinterpret_cast<char*>(meta_buf.data()), meta_size);
    if (!meta_file.good()) {
        return Status::IOError("Failed to read segment.meta");
    }
    meta_file.close();

    // Verify FlatBuffer
    auto verifier = flatbuffers::Verifier(meta_buf.data(), meta_buf.size());
    if (!vdb::schema::VerifySegmentMetaBuffer(verifier)) {
        return Status::Corruption("Invalid segment.meta FlatBuffer");
    }

    const auto* seg_meta = vdb::schema::GetSegmentMeta(meta_buf.data());
    if (!seg_meta) {
        return Status::Corruption("Failed to parse SegmentMeta");
    }

    dim_ = seg_meta->dimension();
    if (dim_ == 0) {
        return Status::InvalidArgument("SegmentMeta dimension is 0");
    }

    // --- 2. Load IVF params ---
    const auto* ivf = seg_meta->ivf_params();
    if (!ivf) {
        return Status::InvalidArgument("SegmentMeta missing ivf_params");
    }
    nlist_ = ivf->nlist();
    if (nlist_ == 0) {
        return Status::InvalidArgument("IvfParams nlist is 0");
    }

    // --- 3. Load centroids.bin ---
    const std::string centroids_path = dir + "/centroids.bin";
    std::ifstream cent_file(centroids_path, std::ios::binary);
    if (!cent_file.is_open()) {
        return Status::IOError("Failed to open centroids.bin: " + centroids_path);
    }
    const size_t cent_size = static_cast<size_t>(nlist_) * dim_ * sizeof(float);
    centroids_.resize(static_cast<size_t>(nlist_) * dim_);
    cent_file.read(reinterpret_cast<char*>(centroids_.data()), cent_size);
    if (!cent_file.good()) {
        return Status::IOError("Failed to read centroids.bin");
    }
    cent_file.close();

    // --- 4. Load rotation.bin ---
    const std::string rotation_path = dir + "/rotation.bin";
    auto rot_result = rabitq::RotationMatrix::Load(rotation_path, dim_);
    if (!rot_result.ok()) {
        return rot_result.status();
    }
    rotation_ = std::make_unique<rabitq::RotationMatrix>(std::move(rot_result.value()));

    // --- 5. Load ConANN params ---
    const auto* conann_params = seg_meta->conann_params();
    if (conann_params) {
        float eps = conann_params->epsilon();
        float dk = conann_params->d_k();
        if (eps > 0.0f) {
            conann_ = ConANN(eps, dk);
        } else {
            // Fallback: use tau_in_factor/tau_out_factor (deprecated path)
            // tau_in_factor = 1 - eps, tau_out_factor = 1 + eps → eps = (tau_out - 1)
            float tau_in = conann_params->tau_in_factor();
            float tau_out = conann_params->tau_out_factor();
            float fallback_eps = (tau_out - tau_in) / 2.0f;
            float fallback_dk = (tau_out + tau_in) / 2.0f;
            conann_ = ConANN(fallback_eps, fallback_dk);
        }
    }

    // --- 6. Register clusters ---
    const auto* clusters = seg_meta->clusters();
    if (clusters) {
        for (uint32_t i = 0; i < clusters->size(); ++i) {
            const auto* cm = clusters->Get(i);
            if (!cm) continue;

            uint32_t cid = cm->cluster_id();
            cluster_ids_.push_back(cid);

            // Build .clu/.dat paths
            char clu_name[32], dat_name[32];
            std::snprintf(clu_name, sizeof(clu_name), "cluster_%04u.clu", cid);
            std::snprintf(dat_name, sizeof(dat_name), "cluster_%04u.dat", cid);
            std::string clu_path = dir + "/" + clu_name;
            std::string dat_path = dir + "/" + dat_name;

            // Read ClusterInfo from .clu trailer
            storage::ClusterStoreWriter::ClusterInfo info;
            auto s = storage::ClusterStoreReader::ReadInfo(clu_path, &info);
            if (!s.ok()) {
                return s;
            }

            s = segment_.AddCluster(info, clu_path, dat_path, dim_,
                                     payload_schemas_);
            if (!s.ok()) {
                return s;
            }
        }
    }

    // Sort cluster_ids for consistent ordering
    std::sort(cluster_ids_.begin(), cluster_ids_.end());

    return Status::OK();
}

// ============================================================================
// FindNearestClusters — brute-force L2 over centroids
// ============================================================================

std::vector<ClusterID> IvfIndex::FindNearestClusters(
    const float* query, uint32_t nprobe) const {

    if (nprobe == 0 || cluster_ids_.empty()) {
        return {};
    }

    const uint32_t actual_nprobe = std::min(nprobe, nlist_);

    // Compute distances to all centroids
    std::vector<std::pair<float, uint32_t>> dists(nlist_);
    for (uint32_t i = 0; i < nlist_; ++i) {
        float d = simd::L2Sqr(query, centroid(i), dim_);
        dists[i] = {d, i};
    }

    // Partial sort to find the nprobe nearest
    std::nth_element(dists.begin(), dists.begin() + actual_nprobe, dists.end(),
                     [](const auto& a, const auto& b) {
                         return a.first < b.first;
                     });

    // Sort the top nprobe by distance for deterministic ordering
    std::sort(dists.begin(), dists.begin() + actual_nprobe,
              [](const auto& a, const auto& b) {
                  return a.first < b.first;
              });

    // Map centroid indices to cluster IDs
    std::vector<ClusterID> result(actual_nprobe);
    for (uint32_t i = 0; i < actual_nprobe; ++i) {
        result[i] = cluster_ids_[dists[i].second];
    }

    return result;
}

}  // namespace index
}  // namespace vdb
