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
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/popcount.h"
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
                         const std::string& output_dir,
                         PayloadFn payload_fn) {
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
    s = WriteIndex(vectors, N, dim, output_dir, std::move(payload_fn));
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
// CalibrateEpsilonIp — global inner-product estimation error bound
// ============================================================================
//
// For each cluster, sample pseudo-queries (vectors from the same cluster) and
// compute |ŝ - s| where ŝ is the popcount-based IP estimate and s is the
// accurate float dot product.  Collect all errors into a global pool and
// return the configured percentile.

static float CalibrateEpsilonIp(
    const std::vector<std::vector<rabitq::RaBitQCode>>& all_codes,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const float* vectors,
    const float* centroids,
    const rabitq::RotationMatrix& rotation,
    Dim dim,
    uint32_t K,
    uint32_t max_samples_per_cluster,
    float percentile,
    uint64_t seed) {

    const uint32_t num_words = (dim + 63) / 64;
    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    rabitq::RaBitQEstimator estimator(dim);

    std::vector<float> ip_errors;

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        const auto& codes = all_codes[k];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        if (n_members < 2) continue;

        // Sampling: pick pseudo-queries from this cluster
        uint32_t n_queries = max_samples_per_cluster;
        if (n_members < 2 * max_samples_per_cluster) {
            n_queries = std::max(n_members / 2, 1u);
        }
        n_queries = std::min(n_queries, n_members);

        std::vector<uint32_t> indices(n_members);
        std::iota(indices.begin(), indices.end(), 0u);
        std::mt19937 rng(seed + k);
        std::shuffle(indices.begin(), indices.end(), rng);

        const float* centroid = centroids + static_cast<size_t>(k) * dim;

        for (uint32_t q = 0; q < n_queries; ++q) {
            const uint32_t q_idx = indices[q];
            const float* query = vectors +
                static_cast<size_t>(members[q_idx]) * dim;

            // PrepareQuery gives us the rotated normalized residual and sign code
            auto pq = estimator.PrepareQuery(query, centroid, rotation);

            // Compare popcount IP vs accurate IP for all other codes in cluster
            for (uint32_t t = 0; t < n_members; ++t) {
                if (t == q_idx) continue;
                const auto& code = codes[t];

                // Popcount path: ŝ = 1 - 2·hamming/dim
                uint32_t hamming = simd::PopcountXor(
                    pq.sign_code.data(), code.code.data(), num_words);
                float ip_hat = 1.0f - 2.0f * static_cast<float>(hamming) /
                                              static_cast<float>(dim);

                // Accurate path: s = (1/√dim) · Σ rotated_q[i] × (2·bit[i]-1)
                float dot = 0.0f;
                for (size_t i = 0; i < dim; ++i) {
                    int bit = (code.code[i / 64] >> (i % 64)) & 1;
                    float sign = 2.0f * bit - 1.0f;
                    dot += pq.rotated[i] * sign;
                }
                float ip_accurate = dot * inv_sqrt_dim;

                ip_errors.push_back(std::abs(ip_hat - ip_accurate));
            }
        }
    }

    if (ip_errors.empty()) return 0.0f;

    std::sort(ip_errors.begin(), ip_errors.end());
    float findex = percentile * static_cast<float>(ip_errors.size() - 1);
    auto idx = static_cast<size_t>(std::min(
        findex, static_cast<float>(ip_errors.size() - 1)));
    return ip_errors[idx];
}

// ============================================================================
// Phase C+D: Write per-cluster files + global metadata
// ============================================================================

Status IvfBuilder::WriteIndex(const float* vectors, uint32_t N, Dim dim,
                              const std::string& output_dir,
                              PayloadFn payload_fn) {
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
            auto pl = payload_fn ? payload_fn(idx) : std::vector<Datum>{};
            s = dat_writer.WriteRecord(vec, pl, entry);
            if (!s.ok()) return s;
            addr_entries_per_cluster[k].push_back(entry);
        }
    }

    s = dat_writer.Finalize();
    if (!s.ok()) return s;

    // --- Phase 2a: RaBitQ encode all clusters, compute r_max ---
    std::vector<std::vector<rabitq::RaBitQCode>> all_codes(K);
    std::vector<float> r_max_per_cluster(K, 0.0f);

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        const float* centroid = centroids_.data() + static_cast<size_t>(k) * dim;

        // Gather member vectors contiguously
        std::vector<float> member_vecs(static_cast<size_t>(n_members) * dim);
        for (uint32_t m = 0; m < n_members; ++m) {
            std::memcpy(member_vecs.data() + static_cast<size_t>(m) * dim,
                         vectors + static_cast<size_t>(members[m]) * dim,
                         dim * sizeof(float));
        }

        all_codes[k] = encoder.EncodeBatch(member_vecs.data(), n_members, centroid);

        // r_max = max(‖o-c‖) in this cluster
        for (const auto& code : all_codes[k]) {
            r_max_per_cluster[k] = std::max(r_max_per_cluster[k], code.norm);
        }
    }

    // --- Phase 2b: calibrate global ε_ip ---
    calibrated_eps_ip_ = CalibrateEpsilonIp(
        all_codes, cluster_members, vectors, centroids_.data(),
        rotation, dim, K,
        config_.epsilon_samples, config_.epsilon_percentile, config_.seed);

    // --- Phase 2c: write unified cluster.clu ---
    storage::ClusterStoreWriter clu_writer;
    s = clu_writer.Open(clu_path, K, dim, config_.rabitq);
    if (!s.ok()) return s;

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        const float* centroid = centroids_.data() + static_cast<size_t>(k) * dim;

        // AddressColumn encode
        auto addr_blocks = storage::AddressColumn::Encode(
            addr_entries_per_cluster[k], 64 /*fixed packed size*/, config_.page_size);

        // Write cluster block (epsilon field stores r_max)
        s = clu_writer.BeginCluster(k, n_members, centroid, r_max_per_cluster[k]);
        if (!s.ok()) return s;

        s = clu_writer.WriteVectors(all_codes[k]);
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
        calibrated_eps_ip_,              // epsilon = ε_ip (inner-product error bound)
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

    // PayloadColumnSchema array
    std::vector<flatbuffers::Offset<vdb::schema::PayloadColumnSchema>> ps_offsets;
    for (const auto& cs : config_.payload_schemas) {
        auto name_off = fbb.CreateString(cs.name);
        ps_offsets.push_back(vdb::schema::CreatePayloadColumnSchema(
            fbb, cs.id, name_off, static_cast<uint8_t>(cs.dtype), cs.nullable));
    }
    auto payload_schemas_vec = fbb.CreateVector(ps_offsets);

    // SegmentMeta (use builder pattern to include payload_schemas)
    vdb::schema::SegmentMetaBuilder smb(fbb);
    smb.add_segment_id(config_.segment_id);
    smb.add_version(1);
    smb.add_state(vdb::schema::SegmentState::ACTIVE);
    smb.add_dimension(dim);
    smb.add_metric_type(vdb::schema::MetricType::L2);
    smb.add_vector_dtype(vdb::schema::VectorDType::FLOAT32);
    smb.add_ivf_params(ivf_params);
    smb.add_rabitq_params(rabitq_params);
    smb.add_conann_params(conann_params);
    smb.add_clusters(clusters_vec);
    smb.add_payload_schemas(payload_schemas_vec);
    auto seg = smb.Finish();

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
