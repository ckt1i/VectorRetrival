#include "vdb/index/ivf_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>

#include "vdb/index/conann.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/data_file_writer.h"

// FlatBuffers generated header
#include "segment_meta_generated.h"

namespace vdb {
namespace index {

// ============================================================================
// Construction / Destruction
// ============================================================================

IvfBuilder::IvfBuilder(const IvfBuilderConfig& config) : config_(config) {}
IvfBuilder::~IvfBuilder() = default;

// ============================================================================
// Build — orchestrate the full build pipeline
// ============================================================================

Status IvfBuilder::Build(const float* vectors, uint32_t N, Dim dim,
                         const std::string& output_dir) {
    if (!vectors || N == 0 || dim == 0) {
        return Status::InvalidArgument("vectors, N, or dim is zero/null");
    }
    if (config_.nlist == 0) {
        return Status::InvalidArgument("nlist must be > 0");
    }
    if (config_.nlist > N) {
        return Status::InvalidArgument("nlist must be <= N");
    }

    // Phase A: K-means clustering
    auto s = RunKMeans(vectors, N, dim);
    if (!s.ok()) return s;

    // Phase B: calibrate ConANN d_k
    CalibrateDk(vectors, N, dim);

    // Phase C+D: write everything to disk
    s = WriteIndex(vectors, N, dim, output_dir);
    return s;
}

// ============================================================================
// Phase A: Capacity-Constrained K-Means
// ============================================================================

Status IvfBuilder::RunKMeans(const float* vectors, uint32_t N, Dim dim) {
    const uint32_t K = config_.nlist;

    // --- Initialization: K-means++ ---
    centroids_.resize(static_cast<size_t>(K) * dim);
    assignments_.resize(N);

    std::mt19937_64 rng(config_.seed);

    // Choose first centroid uniformly at random
    std::uniform_int_distribution<uint32_t> uni(0, N - 1);
    uint32_t first = uni(rng);
    std::memcpy(centroids_.data(), vectors + static_cast<size_t>(first) * dim,
                dim * sizeof(float));

    // K-means++ initialization for remaining centroids
    std::vector<float> min_dist(N, std::numeric_limits<float>::max());
    for (uint32_t k = 1; k < K; ++k) {
        // Update min distances to the newly added centroid k-1
        const float* prev = centroids_.data() + static_cast<size_t>(k - 1) * dim;
        double sum_dist = 0.0;
        for (uint32_t i = 0; i < N; ++i) {
            float d = simd::L2Sqr(vectors + static_cast<size_t>(i) * dim, prev, dim);
            if (d < min_dist[i]) {
                min_dist[i] = d;
            }
            sum_dist += min_dist[i];
        }

        // Weighted sampling proportional to D^2
        std::uniform_real_distribution<double> real_dist(0.0, sum_dist);
        double target = real_dist(rng);
        double running = 0.0;
        uint32_t chosen = N - 1;
        for (uint32_t i = 0; i < N; ++i) {
            running += min_dist[i];
            if (running >= target) {
                chosen = i;
                break;
            }
        }

        std::memcpy(centroids_.data() + static_cast<size_t>(k) * dim,
                     vectors + static_cast<size_t>(chosen) * dim,
                     dim * sizeof(float));
    }

    // --- Iterative refinement ---
    const bool use_balance = config_.balance_factor > 0.0f;
    // Maximum per-cluster capacity when balancing
    const uint32_t max_cap =
        use_balance
            ? static_cast<uint32_t>(
                  std::ceil(static_cast<double>(N) *
                            (1.0 + config_.balance_factor) /
                            static_cast<double>(K)))
            : N;  // no limit

    std::vector<float> new_centroids(static_cast<size_t>(K) * dim);
    std::vector<uint32_t> counts(K);

    for (uint32_t iter = 0; iter < config_.max_iterations; ++iter) {
        // ----- Assignment step -----
        for (uint32_t i = 0; i < N; ++i) {
            const float* vec = vectors + static_cast<size_t>(i) * dim;
            float best_dist = std::numeric_limits<float>::max();
            uint32_t best_k = 0;
            for (uint32_t k = 0; k < K; ++k) {
                float d = simd::L2Sqr(vec, centroids_.data() + static_cast<size_t>(k) * dim, dim);
                if (d < best_dist) {
                    best_dist = d;
                    best_k = k;
                }
            }
            assignments_[i] = best_k;
        }

        // ----- Capacity-constrained reassignment -----
        if (use_balance) {
            // Count current sizes
            std::fill(counts.begin(), counts.end(), 0);
            for (uint32_t i = 0; i < N; ++i) {
                counts[assignments_[i]]++;
            }

            // For each over-capacity cluster, move farthest vectors
            for (uint32_t k = 0; k < K; ++k) {
                if (counts[k] <= max_cap) continue;

                // Gather (distance, index) for members of cluster k
                std::vector<std::pair<float, uint32_t>> members;
                members.reserve(counts[k]);
                const float* ck = centroids_.data() + static_cast<size_t>(k) * dim;
                for (uint32_t i = 0; i < N; ++i) {
                    if (assignments_[i] == k) {
                        float d = simd::L2Sqr(vectors + static_cast<size_t>(i) * dim, ck, dim);
                        members.push_back({d, i});
                    }
                }
                // Sort descending by distance (farthest first)
                std::sort(members.begin(), members.end(),
                          [](const auto& a, const auto& b) {
                              return a.first > b.first;
                          });

                // Move excess vectors to nearest under-capacity cluster
                uint32_t excess = counts[k] - max_cap;
                for (uint32_t e = 0; e < excess && e < members.size(); ++e) {
                    uint32_t vi = members[e].second;
                    const float* vec = vectors + static_cast<size_t>(vi) * dim;

                    // Find nearest under-capacity cluster (excluding k)
                    float best_d = std::numeric_limits<float>::max();
                    uint32_t best_j = k;
                    for (uint32_t j = 0; j < K; ++j) {
                        if (j == k || counts[j] >= max_cap) continue;
                        float d = simd::L2Sqr(vec, centroids_.data() + static_cast<size_t>(j) * dim, dim);
                        if (d < best_d) {
                            best_d = d;
                            best_j = j;
                        }
                    }
                    if (best_j != k) {
                        assignments_[vi] = best_j;
                        counts[k]--;
                        counts[best_j]++;
                    }
                }
            }
        }

        // ----- Update step: recompute centroids -----
        std::fill(new_centroids.begin(), new_centroids.end(), 0.0f);
        std::fill(counts.begin(), counts.end(), 0);

        for (uint32_t i = 0; i < N; ++i) {
            uint32_t k = assignments_[i];
            counts[k]++;
            const float* vec = vectors + static_cast<size_t>(i) * dim;
            float* c = new_centroids.data() + static_cast<size_t>(k) * dim;
            for (Dim d = 0; d < dim; ++d) {
                c[d] += vec[d];
            }
        }

        // Average and check convergence
        float max_shift = 0.0f;
        for (uint32_t k = 0; k < K; ++k) {
            float* c = new_centroids.data() + static_cast<size_t>(k) * dim;
            if (counts[k] > 0) {
                float inv = 1.0f / static_cast<float>(counts[k]);
                for (Dim d = 0; d < dim; ++d) {
                    c[d] *= inv;
                }
            } else {
                // Empty cluster: reinitialize to a random vector
                uint32_t ri = uni(rng);
                std::memcpy(c, vectors + static_cast<size_t>(ri) * dim,
                             dim * sizeof(float));
            }

            float shift = simd::L2Sqr(
                c, centroids_.data() + static_cast<size_t>(k) * dim, dim);
            if (shift > max_shift) max_shift = shift;
        }

        centroids_ = new_centroids;

        // Convergence check
        if (max_shift < config_.tolerance) break;
    }

    return Status::OK();
}

// ============================================================================
// Phase B: Calibrate ConANN d_k
// ============================================================================

void IvfBuilder::CalibrateDk(const float* vectors, uint32_t N, Dim dim) {
    calibrated_dk_ = ConANN::CalibrateDistanceThreshold(
        vectors, N, dim,
        config_.calibration_samples,
        config_.calibration_topk,
        config_.calibration_percentile,
        config_.seed);
}

// ============================================================================
// Phase C+D: Write per-cluster files + global metadata
// ============================================================================

Status IvfBuilder::WriteIndex(const float* vectors, uint32_t N, Dim dim,
                              const std::string& output_dir) {
    const uint32_t K = config_.nlist;

    // --- Create output directory ---
    std::filesystem::create_directories(output_dir);

    // --- Generate rotation matrix ---
    rabitq::RotationMatrix rotation(dim);
    rotation.GenerateRandom(config_.seed);
    auto s = rotation.Save(output_dir + "/rotation.bin");
    if (!s.ok()) return s;

    // --- Write centroids.bin ---
    {
        const std::string path = output_dir + "/centroids.bin";
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError("Failed to create centroids.bin: " + path);
        }
        f.write(reinterpret_cast<const char*>(centroids_.data()),
                static_cast<std::streamsize>(K) * dim * sizeof(float));
        if (!f.good()) {
            return Status::IOError("Failed to write centroids.bin");
        }
    }

    // --- Group vectors by cluster ---
    std::vector<std::vector<uint32_t>> cluster_members(K);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments_[i]].push_back(i);
    }

    // --- Compute epsilon for ConANN ---
    float epsilon = config_.rabitq.c_factor *
                    std::pow(2.0f, -static_cast<float>(config_.rabitq.bits) / 2.0f) /
                    std::sqrt(static_cast<float>(dim));

    // --- Build encoder ---
    rabitq::RaBitQEncoder encoder(dim, rotation);

    // =======================================================================
    // Unified file writing: single data.dat + single cluster.clu
    // =======================================================================

    const std::string dat_path = output_dir + "/data.dat";
    const std::string clu_path = output_dir + "/cluster.clu";

    // --- Phase 1: write all records into one DataFileWriter ---
    storage::DataFileWriter dat_writer;
    s = dat_writer.Open(dat_path, 0, dim, config_.payload_schemas,
                        config_.page_size);
    if (!s.ok()) return s;

    // addr_entries_per_cluster[k] = address entries for cluster k
    std::vector<std::vector<AddressEntry>> addr_entries_per_cluster(K);

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        addr_entries_per_cluster[k].reserve(members.size());

        for (uint32_t idx : members) {
            const float* vec = vectors + static_cast<size_t>(idx) * dim;
            AddressEntry entry;
            s = dat_writer.WriteRecord(vec, {}, entry);
            if (!s.ok()) return s;
            addr_entries_per_cluster[k].push_back(entry);
        }
    }

    s = dat_writer.Finalize();
    if (!s.ok()) return s;

    // --- Phase 2: write unified cluster.clu ---
    storage::ClusterStoreWriter clu_writer;
    s = clu_writer.Open(clu_path, K, dim, config_.rabitq);
    if (!s.ok()) return s;

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        const float* centroid = centroids_.data() + static_cast<size_t>(k) * dim;

        // RaBitQ encode all members (gather contiguously)
        std::vector<float> member_vecs(static_cast<size_t>(n_members) * dim);
        for (uint32_t m = 0; m < n_members; ++m) {
            std::memcpy(member_vecs.data() + static_cast<size_t>(m) * dim,
                         vectors + static_cast<size_t>(members[m]) * dim,
                         dim * sizeof(float));
        }

        auto codes = encoder.EncodeBatch(member_vecs.data(), n_members, centroid);

        // AddressColumn encode
        auto addr_blocks = storage::AddressColumn::Encode(
            addr_entries_per_cluster[k], 64 /*fixed packed size*/, config_.page_size);

        // Write cluster block
        s = clu_writer.BeginCluster(k, n_members, centroid);
        if (!s.ok()) return s;

        s = clu_writer.WriteVectors(codes);
        if (!s.ok()) return s;

        s = clu_writer.WriteAddressBlocks(addr_blocks);
        if (!s.ok()) return s;

        s = clu_writer.EndCluster();
        if (!s.ok()) return s;

        if (progress_cb_) {
            progress_cb_(k, K);
        }
    }

    s = clu_writer.Finalize("data.dat");
    if (!s.ok()) return s;

    // --- Write segment.meta (FlatBuffers) ---
    flatbuffers::FlatBufferBuilder fbb(4096);

    // IvfParams
    auto ivf_params = vdb::schema::CreateIvfParams(
        fbb,
        K,                         // nlist
        config_.nprobe,            // nprobe
        0, 0,                      // centroids_offset/length (using separate file)
        N,                         // training_vectors
        config_.max_iterations,    // kmeans_iterations
        0, 0, 0.0f,               // min/max/avg list size (not computed here)
        config_.balance_factor     // balance_factor
    );

    // RaBitQParams
    auto rabitq_params = vdb::schema::CreateRaBitQParams(
        fbb,
        config_.rabitq.bits,
        config_.rabitq.block_size,
        config_.rabitq.c_factor,
        0, 0  // codebook offset/length (not applicable for 1-bit)
    );

    // ConANNParams
    auto conann_params = vdb::schema::CreateConANNParams(
        fbb,
        0.0f, 0.0f,                     // deprecated tau_in/tau_out factors
        epsilon,                         // epsilon
        calibrated_dk_,                  // d_k
        config_.calibration_samples,     // calibration_samples
        config_.calibration_topk,        // calibration_topk
        config_.calibration_percentile   // calibration_percentile
    );

    // ClusterMeta array — per-cluster info from the lookup table
    const auto& global_info = clu_writer.info();
    std::vector<flatbuffers::Offset<vdb::schema::ClusterMeta>> cluster_offsets;
    cluster_offsets.reserve(K);
    for (uint32_t k = 0; k < K; ++k) {
        const auto& entry = global_info.lookup_table[k];

        // Build empty AddressColumnMeta (address info is in .clu mini-trailers)
        auto ab_vec = fbb.CreateVector(
            std::vector<flatbuffers::Offset<vdb::schema::AddressBlockMeta>>{});
        auto addr_col = vdb::schema::CreateAddressColumnMeta(
            fbb, 64 /*granularity*/, 0, ab_vec);

        auto dfp = fbb.CreateString("data.dat");

        cluster_offsets.push_back(vdb::schema::CreateClusterMeta(
            fbb,
            entry.cluster_id,
            entry.num_records,
            0, 0,                   // centroid_offset/length (in .clu lookup table)
            entry.block_offset,     // rabitq_data_offset → block_offset
            entry.block_size,       // rabitq_data_length → block_size
            addr_col,
            dfp,
            0  // checksum
        ));
    }
    auto clusters_vec = fbb.CreateVector(cluster_offsets);

    // SegmentMeta
    auto seg = vdb::schema::CreateSegmentMeta(
        fbb,
        config_.segment_id,                         // segment_id
        1,                                           // version
        vdb::schema::SegmentState::ACTIVE,           // state
        dim,                                         // dimension
        vdb::schema::MetricType::L2,                 // metric_type
        vdb::schema::VectorDType::FLOAT32,           // vector_dtype
        ivf_params,
        0,    // pq_params
        0,    // opq_params
        0,    // hnsw_params
        rabitq_params,
        conann_params,
        clusters_vec
    );

    fbb.Finish(seg, vdb::schema::SegmentMetaIdentifier());

    // Write segment.meta
    {
        const std::string meta_path = output_dir + "/segment.meta";
        std::ofstream f(meta_path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError("Failed to create segment.meta: " + meta_path);
        }
        f.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                fbb.GetSize());
        if (!f.good()) {
            return Status::IOError("Failed to write segment.meta");
        }
    }

    return Status::OK();
}

}  // namespace index
}  // namespace vdb
