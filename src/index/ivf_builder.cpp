#include "vdb/index/ivf_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>

#include "superkmeans/superkmeans.h"

#include "vdb/index/conann.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/io/vecs_reader.h"
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
// Phase A: Clustering (SuperKMeans or precomputed)
// ============================================================================

Status IvfBuilder::RunKMeans(const float* vectors, uint32_t N, Dim dim) {
    const uint32_t K = config_.nlist;

    // Path 1: Load precomputed centroids and assignments
    if (!config_.centroids_path.empty() && !config_.assignments_path.empty()) {
        auto c_or = io::LoadVectors(config_.centroids_path);
        if (!c_or.ok()) {
            return Status::IOError("Failed to load centroids: " +
                                   c_or.status().ToString());
        }
        auto& c = c_or.value();
        if (c.cols != dim) {
            return Status::InvalidArgument("Centroid dim mismatch");
        }
        centroids_.assign(c.data.begin(), c.data.end());

        auto a_or = io::LoadIvecs(config_.assignments_path);
        if (!a_or.ok()) {
            return Status::IOError("Failed to load assignments: " +
                                   a_or.status().ToString());
        }
        auto& a = a_or.value();
        if (a.rows != N) {
            return Status::InvalidArgument("Assignment count mismatch");
        }
        assignments_.resize(N);
        for (uint32_t i = 0; i < N; ++i) {
            assignments_[i] = static_cast<uint32_t>(a.data[i]);
        }
        return Status::OK();
    }

    // Path 2: SuperKMeans automatic clustering
    skmeans::SuperKMeansConfig skm_cfg;
    skm_cfg.iters = config_.max_iterations;
    skm_cfg.seed = static_cast<uint32_t>(config_.seed);
    skm_cfg.verbose = false;
    skm_cfg.early_termination = true;
    skm_cfg.tol = config_.tolerance;

    auto skm = skmeans::SuperKMeans(K, dim, skm_cfg);
    auto c = skm.Train(vectors, N);
    auto a = skm.Assign(vectors, c.data(), N, K);

    centroids_.assign(c.begin(), c.end());
    assignments_.assign(a.begin(), a.end());

    return Status::OK();
}

// ============================================================================
// Phase B: Calibrate ConANN d_k
// ============================================================================

void IvfBuilder::CalibrateDk(const float* vectors, uint32_t N, Dim dim) {
    if (config_.calibration_queries != nullptr &&
        config_.num_calibration_queries > 0) {
        // Cross-modal: calibrate d_k from query→database distances
        calibrated_dk_ = ConANN::CalibrateDistanceThreshold(
            config_.calibration_queries, config_.num_calibration_queries,
            vectors, N, dim,
            config_.calibration_samples,
            config_.calibration_topk,
            config_.calibration_percentile,
            config_.seed);
    } else {
        // Same-modal fallback: database self-sampling
        calibrated_dk_ = ConANN::CalibrateDistanceThreshold(
            vectors, N, dim,
            config_.calibration_samples,
            config_.calibration_topk,
            config_.calibration_percentile,
            config_.seed);
    }
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
        0.0f                       // balance_factor (deprecated, always 0)
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

    // --- Phase E: CRC score precomputation (optional) ---
    flatbuffers::Offset<vdb::schema::CrcParams> crc_params_offset;
    if (config_.crc_top_k > 0 && config_.calibration_queries != nullptr &&
        config_.num_calibration_queries > 0) {
        // Build ClusterData for CRC score computation.
        // Flatten RaBitQCode objects into contiguous byte blocks.
        const uint32_t num_words = (dim + 63) / 64;
        const uint32_t entry_size = num_words * sizeof(uint64_t) +
                                    sizeof(float) + sizeof(uint32_t);

        std::vector<std::vector<uint8_t>> flat_codes(K);
        std::vector<std::vector<uint32_t>> cluster_ids(K);
        std::vector<ClusterData> crc_clusters(K);

        for (uint32_t k = 0; k < K; ++k) {
            const auto& members = cluster_members[k];
            uint32_t n_members = static_cast<uint32_t>(members.size());

            // Build flat code block
            flat_codes[k].resize(static_cast<size_t>(n_members) * entry_size);
            for (uint32_t m = 0; m < n_members; ++m) {
                uint8_t* dst = flat_codes[k].data() +
                               static_cast<size_t>(m) * entry_size;
                const auto& code = all_codes[k][m];
                std::memcpy(dst, code.code.data(),
                            num_words * sizeof(uint64_t));
                std::memcpy(dst + num_words * sizeof(uint64_t),
                            &code.norm, sizeof(float));
                std::memcpy(dst + num_words * sizeof(uint64_t) + sizeof(float),
                            &code.sum_x, sizeof(uint32_t));
            }

            // Build global IDs
            cluster_ids[k].resize(n_members);
            for (uint32_t m = 0; m < n_members; ++m) {
                cluster_ids[k][m] = members[m];
            }

            crc_clusters[k].vectors = nullptr;  // not needed for RaBitQ scoring
            crc_clusters[k].ids = cluster_ids[k].data();
            crc_clusters[k].count = n_members;
            crc_clusters[k].codes_block = flat_codes[k].data();
            crc_clusters[k].code_entry_size = entry_size;
        }

        // Compute QueryScores for all calibration queries
        auto crc_scores = CrcCalibrator::ComputeScoresRaBitQ(
            config_.calibration_queries, config_.num_calibration_queries,
            dim, centroids_.data(), K, crc_clusters,
            config_.crc_top_k, rotation);

        // Serialize to crc_scores.bin
        s = CrcCalibrator::WriteScores(
            output_dir + "/crc_scores.bin", crc_scores, K, config_.crc_top_k);
        if (!s.ok()) return s;

        // Build FlatBuffers CrcParams
        auto sf = fbb.CreateString("crc_scores.bin");
        crc_params_offset = vdb::schema::CreateCrcParams(
            fbb, sf, config_.num_calibration_queries, config_.crc_top_k);
    }

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
    if (crc_params_offset.o) {
        smb.add_crc_params(crc_params_offset);
    }
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
