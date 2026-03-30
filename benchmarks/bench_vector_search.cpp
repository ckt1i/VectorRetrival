/// bench_vector_search.cpp — Pure vector search benchmark.
///
/// Supports standard ANN-Benchmark datasets (.fvecs/.npy) without payload.
/// Pipeline: Load → KMeans → RaBitQ Encode → Calibrate → Probe+Rerank → Recall.
/// Optionally integrates CRC early-stop in the cluster probing loop.
///
/// Usage:
///   bench_vector_search --base /path/to/base.fvecs --query /path/to/query.fvecs
///       [--gt /path/to/gt.ivecs] [--nlist 256] [--nprobe 32] [--topk 10]
///       [--crc 1] [--crc-alpha 0.1] [--early-stop 1] [--outdir ./results]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/popcount.h"

using namespace vdb;
using namespace vdb::rabitq;
using namespace vdb::index;

namespace fs = std::filesystem;

// ============================================================================
// CLI helpers
// ============================================================================

static std::string GetArg(int argc, char* argv[], const char* name,
                          const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

static int GetIntArg(int argc, char* argv[], const char* name, int def) {
    auto s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::atoi(s.c_str());
}

static float GetFloatArg(int argc, char* argv[], const char* name, float def) {
    auto s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::strtof(s.c_str(), nullptr);
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// KMeans (reused from bench_rabitq_diagnostic)
// ============================================================================

static void KMeans(const float* data, uint32_t N, Dim dim, uint32_t K,
                   uint32_t max_iter, uint64_t seed,
                   std::vector<float>& centroids,
                   std::vector<uint32_t>& assignments) {
    centroids.resize(static_cast<size_t>(K) * dim);
    assignments.resize(N);

    std::mt19937_64 rng(seed);
    std::vector<uint32_t> indices(N);
    std::iota(indices.begin(), indices.end(), 0u);
    std::shuffle(indices.begin(), indices.end(), rng);
    for (uint32_t k = 0; k < K; ++k) {
        std::memcpy(centroids.data() + static_cast<size_t>(k) * dim,
                    data + static_cast<size_t>(indices[k]) * dim,
                    dim * sizeof(float));
    }

    std::vector<uint32_t> counts(K);
    std::vector<float> new_centroids(static_cast<size_t>(K) * dim);

    for (uint32_t iter = 0; iter < max_iter; ++iter) {
        for (uint32_t i = 0; i < N; ++i) {
            const float* v = data + static_cast<size_t>(i) * dim;
            float best_dist = std::numeric_limits<float>::max();
            uint32_t best_k = 0;
            for (uint32_t k = 0; k < K; ++k) {
                float d = simd::L2Sqr(v, centroids.data() + static_cast<size_t>(k) * dim, dim);
                if (d < best_dist) { best_dist = d; best_k = k; }
            }
            assignments[i] = best_k;
        }
        std::fill(counts.begin(), counts.end(), 0);
        std::fill(new_centroids.begin(), new_centroids.end(), 0.0f);
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t k = assignments[i];
            counts[k]++;
            const float* v = data + static_cast<size_t>(i) * dim;
            float* c = new_centroids.data() + static_cast<size_t>(k) * dim;
            for (uint32_t d = 0; d < dim; ++d) c[d] += v[d];
        }
        for (uint32_t k = 0; k < K; ++k) {
            if (counts[k] == 0) continue;
            float* c = new_centroids.data() + static_cast<size_t>(k) * dim;
            float inv = 1.0f / static_cast<float>(counts[k]);
            for (uint32_t d = 0; d < dim; ++d) c[d] *= inv;
        }
        centroids = new_centroids;
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // --- CLI ---
    std::string base_path  = GetArg(argc, argv, "--base", "");
    std::string query_path = GetArg(argc, argv, "--query", "");
    std::string gt_path    = GetArg(argc, argv, "--gt", "");
    uint32_t nlist   = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 256));
    uint32_t nprobe  = static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 32));
    uint32_t top_k   = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    bool use_crc     = GetIntArg(argc, argv, "--crc", 0) != 0;
    float crc_alpha  = GetFloatArg(argc, argv, "--crc-alpha", 0.1f);
    float crc_calib  = GetFloatArg(argc, argv, "--crc-calib", 0.5f);
    float crc_tune   = GetFloatArg(argc, argv, "--crc-tune", 0.1f);
    bool early_stop  = GetIntArg(argc, argv, "--early-stop", 1) != 0;
    uint64_t seed    = static_cast<uint64_t>(GetIntArg(argc, argv, "--seed", 42));
    std::string outdir = GetArg(argc, argv, "--outdir", "");
    int q_limit      = GetIntArg(argc, argv, "--queries", 0);
    uint32_t p_for_dk  = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-dk", 90));
    uint32_t p_for_eps = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-eps", 95));

    if (base_path.empty() || query_path.empty()) {
        std::fprintf(stderr,
            "Usage: bench_vector_search --base <path> --query <path> "
            "[--gt <path>] [--nlist N] [--nprobe N] [--topk K] "
            "[--crc 0|1] [--early-stop 0|1] [--outdir dir]\n");
        return 1;
    }

    Log("=== Vector Search Benchmark ===\n");
    Log("Base:   %s\n", base_path.c_str());
    Log("Query:  %s\n", query_path.c_str());
    Log("GT:     %s\n", gt_path.empty() ? "(brute-force)" : gt_path.c_str());
    Log("nlist=%u  nprobe=%u  top_k=%u  crc=%d  early_stop=%d\n",
        nlist, nprobe, top_k, use_crc, early_stop);

    // ================================================================
    // Phase A: Load data
    // ================================================================
    Log("\n[Phase A] Loading data...\n");

    auto base_or = io::LoadVectors(base_path);
    if (!base_or.ok()) {
        std::fprintf(stderr, "Failed to load base: %s\n",
                     base_or.status().ToString().c_str());
        return 1;
    }
    auto& base = base_or.value();
    uint32_t N = base.rows;
    Dim dim = static_cast<Dim>(base.cols);

    auto qry_or = io::LoadVectors(query_path);
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load query: %s\n",
                     qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q = (q_limit > 0) ? std::min(static_cast<uint32_t>(q_limit), qry.rows)
                                : qry.rows;
    if (qry.cols != base.cols) {
        std::fprintf(stderr, "Dimension mismatch: base=%u, query=%u\n",
                     base.cols, qry.cols);
        return 1;
    }

    Log("  N=%u, Q=%u, dim=%u\n", N, Q, dim);

    // GT loading
    std::vector<std::vector<uint32_t>> gt_ids;  // [Q][gt_k]
    uint32_t gt_k = 0;

    if (!gt_path.empty() && fs::exists(gt_path)) {
        Log("  Loading GT from %s...\n", gt_path.c_str());
        auto gt_or = io::LoadIvecs(gt_path);
        if (!gt_or.ok()) {
            std::fprintf(stderr, "Failed to load GT: %s\n",
                         gt_or.status().ToString().c_str());
            return 1;
        }
        auto& gt = gt_or.value();
        gt_k = gt.cols;
        uint32_t gt_rows = std::min(gt.rows, Q);
        gt_ids.resize(gt_rows);
        for (uint32_t qi = 0; qi < gt_rows; ++qi) {
            gt_ids[qi].assign(
                gt.data.data() + static_cast<size_t>(qi) * gt.cols,
                gt.data.data() + static_cast<size_t>(qi) * gt.cols + gt.cols);
        }
        Log("  GT: %u queries, k=%u\n", gt_rows, gt_k);
    } else {
        Log("  No GT file — computing brute-force L2...\n");
        gt_k = std::max(top_k, 100u);
        gt_ids.resize(Q);
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
            std::vector<std::pair<float, uint32_t>> dists(N);
            for (uint32_t i = 0; i < N; ++i) {
                dists[i] = {simd::L2Sqr(q_vec,
                    base.data.data() + static_cast<size_t>(i) * dim, dim), i};
            }
            std::partial_sort(dists.begin(),
                              dists.begin() + std::min(gt_k, N), dists.end());
            gt_ids[qi].resize(std::min(gt_k, N));
            for (uint32_t i = 0; i < std::min(gt_k, N); ++i) {
                gt_ids[qi][i] = dists[i].second;
            }
        }
        Log("  Brute-force GT computed (k=%u).\n", gt_k);
    }

    // ================================================================
    // Phase B: Build — KMeans + RaBitQ Encode + calibrate ε_ip/d_k
    // ================================================================
    Log("\n[Phase B] KMeans (K=%u) + RaBitQ encoding...\n", nlist);
    auto t_build = std::chrono::steady_clock::now();

    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    KMeans(base.data.data(), N, dim, nlist, 20, seed, centroids, assignments);

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(seed);
    RaBitQEncoder encoder(dim, rotation);
    RaBitQEstimator estimator(dim);

    std::vector<RaBitQCode> codes(N);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t k = assignments[i];
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        codes[i] = encoder.Encode(
            base.data.data() + static_cast<size_t>(i) * dim, centroid);
    }

    // Cluster members
    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    // Per-cluster r_max
    std::vector<float> per_cluster_r_max(nlist, 0.0f);
    for (uint32_t k = 0; k < nlist; ++k) {
        for (uint32_t idx : cluster_members[k]) {
            per_cluster_r_max[k] = std::max(per_cluster_r_max[k], codes[idx].norm);
        }
    }

    // ε_ip calibration (from intra-cluster inner-product error)
    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    const uint32_t num_words = (dim + 63) / 64;
    uint32_t samples_for_eps = 100;

    std::vector<float> ip_errors;
    for (uint32_t k = 0; k < nlist; ++k) {
        const auto& members = cluster_members[k];
        if (members.size() < 2) continue;
        uint32_t n_queries = std::min(samples_for_eps,
                                       static_cast<uint32_t>(members.size()));
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        std::vector<uint32_t> indices(members.size());
        std::iota(indices.begin(), indices.end(), 0u);
        std::mt19937 rng(seed + k);
        std::shuffle(indices.begin(), indices.end(), rng);

        for (uint32_t q = 0; q < n_queries; ++q) {
            uint32_t q_global = members[indices[q]];
            const float* q_vec = base.data.data() +
                static_cast<size_t>(q_global) * dim;
            auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            for (uint32_t t = 0; t < static_cast<uint32_t>(members.size()); ++t) {
                if (t == indices[q]) continue;
                uint32_t t_global = members[t];
                const auto& code = codes[t_global];
                uint32_t hamming = simd::PopcountXor(
                    pq.sign_code.data(), code.code.data(), num_words);
                float ip_hat = 1.0f - 2.0f * static_cast<float>(hamming) /
                                              static_cast<float>(dim);
                float dot = 0.0f;
                for (size_t d = 0; d < dim; ++d) {
                    int bit = (code.code[d / 64] >> (d % 64)) & 1;
                    dot += pq.rotated[d] * (2.0f * bit - 1.0f);
                }
                float ip_accurate = dot * inv_sqrt_dim;
                ip_errors.push_back(std::abs(ip_hat - ip_accurate));
            }
        }
    }
    float eps_ip = 0.0f;
    if (!ip_errors.empty()) {
        std::sort(ip_errors.begin(), ip_errors.end());
        float p = static_cast<float>(p_for_eps) / 100.0f;
        auto idx = static_cast<size_t>(
            p * static_cast<float>(ip_errors.size() - 1));
        eps_ip = ip_errors[idx];
    }
    Log("  eps_ip = %.6f (P%u)\n", eps_ip, p_for_eps);

    // d_k calibration
    float p_dk_f = static_cast<float>(p_for_dk) / 100.0f;
    float d_k = ConANN::CalibrateDistanceThreshold(
        qry.data.data(), Q, base.data.data(), N,
        dim, 100, top_k, p_dk_f, seed);
    Log("  d_k = %.4f (P%u)\n", d_k, p_for_dk);

    double build_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_build).count();
    Log("  Build complete (%.1f ms).\n", build_ms);

    // ================================================================
    // Phase C: CRC Calibration (optional)
    // ================================================================
    CrcStopper crc_stopper;
    CalibrationResults calib_results;
    bool crc_active = false;

    if (use_crc) {
        Log("\n[Phase C] CRC calibration (inline RaBitQ)...\n");
        auto t_crc = std::chrono::steady_clock::now();

        // Build ClusterData for CalibrateWithRaBitQ
        // Pack codes into contiguous blocks per cluster
        uint32_t code_words = (dim + 63) / 64;
        uint32_t code_entry_size = code_words * sizeof(uint64_t) + sizeof(float) + sizeof(uint32_t);

        std::vector<std::vector<uint8_t>> cluster_code_blocks(nlist);
        std::vector<std::vector<uint32_t>> cluster_ids(nlist);

        for (uint32_t cid = 0; cid < nlist; ++cid) {
            const auto& members = cluster_members[cid];
            cluster_ids[cid] = members;
            cluster_code_blocks[cid].resize(members.size() * code_entry_size);

            for (size_t j = 0; j < members.size(); ++j) {
                uint8_t* dst = cluster_code_blocks[cid].data() + j * code_entry_size;
                const auto& code = codes[members[j]];
                // Pack: code words || norm || vector_id
                std::memcpy(dst, code.code.data(), code_words * sizeof(uint64_t));
                dst += code_words * sizeof(uint64_t);
                std::memcpy(dst, &code.norm, sizeof(float));
                dst += sizeof(float);
                uint32_t vid = members[j];
                std::memcpy(dst, &vid, sizeof(uint32_t));
            }
        }

        std::vector<ClusterData> clusters(nlist);
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            clusters[cid].vectors = nullptr;
            clusters[cid].ids = cluster_ids[cid].data();
            clusters[cid].count = static_cast<uint32_t>(cluster_members[cid].size());
            clusters[cid].codes_block = cluster_code_blocks[cid].data();
            clusters[cid].code_entry_size = code_entry_size;
        }

        CrcCalibrator::Config crc_cfg;
        crc_cfg.alpha = crc_alpha;
        crc_cfg.top_k = top_k;
        crc_cfg.calib_ratio = crc_calib;
        crc_cfg.tune_ratio = crc_tune;
        crc_cfg.seed = seed;

        auto [cal, eval] = CrcCalibrator::CalibrateWithRaBitQ(
            crc_cfg, qry.data.data(), Q, dim,
            centroids.data(), nlist, clusters, rotation);
        calib_results = cal;
        crc_stopper = CrcStopper(calib_results, nlist);
        crc_active = (cal.lamhat < 1.0f);

        double crc_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_crc).count();

        Log("  CRC done (%.1f ms)\n", crc_ms);
        Log("  lamhat=%.6f  kreg=%u  reg_lambda=%.6f\n",
            cal.lamhat, cal.kreg, cal.reg_lambda);
        Log("  d_min=%.6f  d_max=%.6f\n", cal.d_min, cal.d_max);
        Log("  eval: FNR=%.4f  avg_probed=%.1f  test_size=%u\n",
            eval.actual_fnr, eval.avg_probed, eval.test_size);
        Log("  CRC active: %s\n", crc_active ? "yes" : "no (lamhat >= 1.0)");
    }

    // ================================================================
    // Phase D: Vector Search
    // ================================================================
    Log("\n[Phase D] Searching (%u queries, nprobe=%u)...\n", Q, nprobe);
    auto t_search = std::chrono::steady_clock::now();

    ConANN conann(eps_ip, d_k);

    // Results: per-query top-K IDs
    std::vector<std::vector<uint32_t>> results(Q);
    std::vector<double> latencies(Q);

    // Stats
    uint64_t total_safein = 0, total_safeout = 0, total_uncertain = 0;
    uint64_t total_probed_clusters = 0;
    uint32_t early_stop_count = 0;

    for (uint32_t qi = 0; qi < Q; ++qi) {
        auto t_q = std::chrono::steady_clock::now();
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;

        // Sort clusters by centroid distance
        std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
        for (uint32_t c = 0; c < nlist; ++c) {
            centroid_dists[c] = {
                simd::L2Sqr(q_vec,
                    centroids.data() + static_cast<size_t>(c) * dim, dim),
                c};
        }
        std::sort(centroid_dists.begin(), centroid_dists.end());

        // Dual heaps: max-heap (largest at front)
        std::vector<std::pair<float, uint32_t>> est_heap;
        std::vector<std::pair<float, uint32_t>> exact_heap;

        uint32_t probed = 0;
        bool stopped_early = false;

        for (uint32_t p = 0; p < nprobe && p < nlist; ++p) {
            uint32_t cid = centroid_dists[p].second;
            const float* centroid = centroids.data() +
                static_cast<size_t>(cid) * dim;
            auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            float margin = 2.0f * per_cluster_r_max[cid] * pq.norm_qc * eps_ip;

            for (uint32_t vid : cluster_members[cid]) {
                float est_dist = estimator.EstimateDistance(pq, codes[vid]);

                // ConANN classification
                float est_kth = (est_heap.size() >= top_k)
                    ? est_heap.front().first
                    : std::numeric_limits<float>::infinity();
                ResultClass rc = conann.ClassifyAdaptive(est_dist, margin, est_kth);

                if (rc == ResultClass::SafeOut) {
                    total_safeout++;
                    continue;
                }
                if (rc == ResultClass::SafeIn) total_safein++;
                else total_uncertain++;

                // Update est_heap
                if (est_heap.size() < top_k) {
                    est_heap.push_back({est_dist, vid});
                    std::push_heap(est_heap.begin(), est_heap.end());
                } else if (est_dist < est_heap.front().first) {
                    std::pop_heap(est_heap.begin(), est_heap.end());
                    est_heap.back() = {est_dist, vid};
                    std::push_heap(est_heap.begin(), est_heap.end());
                }

                // Rerank: exact L2
                float exact_dist = simd::L2Sqr(q_vec,
                    base.data.data() + static_cast<size_t>(vid) * dim, dim);
                if (exact_heap.size() < top_k) {
                    exact_heap.push_back({exact_dist, vid});
                    std::push_heap(exact_heap.begin(), exact_heap.end());
                } else if (exact_dist < exact_heap.front().first) {
                    std::pop_heap(exact_heap.begin(), exact_heap.end());
                    exact_heap.back() = {exact_dist, vid};
                    std::push_heap(exact_heap.begin(), exact_heap.end());
                }
            }

            probed++;

            // Early stop: CRC or legacy d_k
            if (use_crc && crc_active) {
                float est_kth = (est_heap.size() >= top_k)
                    ? est_heap.front().first
                    : std::numeric_limits<float>::infinity();
                if (crc_stopper.ShouldStop(probed, est_kth)) {
                    stopped_early = true;
                    break;
                }
            } else if (early_stop) {
                if (exact_heap.size() >= top_k && exact_heap.front().first < d_k) {
                    stopped_early = true;
                    break;
                }
            }
        }

        total_probed_clusters += probed;
        if (stopped_early) early_stop_count++;

        // Extract top-K from exact_heap, sorted by distance (ascending)
        std::sort_heap(exact_heap.begin(), exact_heap.end());
        // After sort_heap on a max-heap, elements are in ascending order
        results[qi].resize(exact_heap.size());
        for (size_t i = 0; i < exact_heap.size(); ++i) {
            results[qi][i] = exact_heap[i].second;
        }

        latencies[qi] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_q).count();
    }

    double search_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_search).count();
    Log("  Search complete (%.1f ms).\n", search_ms);

    // ================================================================
    // Phase E: Recall & Statistics
    // ================================================================
    Log("\n[Phase E] Computing recall...\n");

    auto ComputeRecall = [&](uint32_t at_k) -> double {
        if (at_k == 0) return 0.0;
        uint32_t valid = 0;
        double sum = 0.0;
        for (uint32_t qi = 0; qi < Q && qi < gt_ids.size(); ++qi) {
            std::unordered_set<uint32_t> gt_set;
            uint32_t gt_limit = std::min(at_k, static_cast<uint32_t>(gt_ids[qi].size()));
            for (uint32_t i = 0; i < gt_limit; ++i) {
                gt_set.insert(static_cast<uint32_t>(gt_ids[qi][i]));
            }
            if (gt_set.empty()) continue;

            uint32_t hits = 0;
            uint32_t check_limit = std::min(at_k, static_cast<uint32_t>(results[qi].size()));
            for (uint32_t i = 0; i < check_limit; ++i) {
                if (gt_set.count(results[qi][i])) hits++;
            }
            sum += static_cast<double>(hits) / static_cast<double>(gt_set.size());
            valid++;
        }
        return valid > 0 ? sum / valid : 0.0;
    };

    double recall_1   = ComputeRecall(1);
    double recall_5   = ComputeRecall(std::min(5u, top_k));
    double recall_10  = ComputeRecall(std::min(10u, top_k));
    double recall_100 = ComputeRecall(std::min(100u, top_k));

    // Latency stats
    std::vector<double> sorted_lat = latencies;
    std::sort(sorted_lat.begin(), sorted_lat.end());
    double avg_lat = std::accumulate(sorted_lat.begin(), sorted_lat.end(), 0.0) / Q;
    double p50 = sorted_lat[Q / 2];
    double p95 = sorted_lat[static_cast<size_t>(Q * 0.95)];
    double p99 = sorted_lat[static_cast<size_t>(Q * 0.99)];

    double avg_probed = static_cast<double>(total_probed_clusters) / Q;
    double early_stop_rate = static_cast<double>(early_stop_count) / Q;

    uint64_t total_classified = total_safein + total_safeout + total_uncertain;

    // ================================================================
    // Phase F: Output
    // ================================================================
    Log("\n========================================\n");
    Log("RESULTS\n");
    Log("========================================\n");
    Log("  recall@1  = %.4f\n", recall_1);
    Log("  recall@5  = %.4f\n", recall_5);
    Log("  recall@10 = %.4f\n", recall_10);
    if (top_k >= 100)
        Log("  recall@100= %.4f\n", recall_100);
    Log("\n");
    Log("  latency avg = %.3f ms\n", avg_lat);
    Log("  latency p50 = %.3f ms\n", p50);
    Log("  latency p95 = %.3f ms\n", p95);
    Log("  latency p99 = %.3f ms\n", p99);
    Log("\n");
    Log("  avg_probed      = %.2f / %u clusters\n", avg_probed, nprobe);
    Log("  early_stop_rate = %.4f\n", early_stop_rate);
    Log("\n");
    if (total_classified > 0) {
        Log("  ConANN stats:\n");
        Log("    SafeIn    = %lu (%.2f%%)\n", total_safein,
            100.0 * total_safein / total_classified);
        Log("    SafeOut   = %lu (%.2f%%)\n", total_safeout,
            100.0 * total_safeout / total_classified);
        Log("    Uncertain = %lu (%.2f%%)\n", total_uncertain,
            100.0 * total_uncertain / total_classified);
    }
    if (use_crc) {
        Log("\n  CRC stats:\n");
        Log("    lamhat     = %.6f\n", calib_results.lamhat);
        Log("    crc_active = %s\n", crc_active ? "yes" : "no");
    }
    Log("========================================\n");

    // JSON output
    if (!outdir.empty()) {
        fs::create_directories(outdir);
        std::string json_path = outdir + "/results.json";
        std::ofstream jf(json_path);
        jf << "{\n";
        jf << "  \"base\": \"" << base_path << "\",\n";
        jf << "  \"query\": \"" << query_path << "\",\n";
        jf << "  \"N\": " << N << ",\n";
        jf << "  \"Q\": " << Q << ",\n";
        jf << "  \"dim\": " << dim << ",\n";
        jf << "  \"nlist\": " << nlist << ",\n";
        jf << "  \"nprobe\": " << nprobe << ",\n";
        jf << "  \"top_k\": " << top_k << ",\n";
        jf << "  \"crc\": " << (use_crc ? "true" : "false") << ",\n";
        jf << "  \"recall_at_1\": " << recall_1 << ",\n";
        jf << "  \"recall_at_5\": " << recall_5 << ",\n";
        jf << "  \"recall_at_10\": " << recall_10 << ",\n";
        jf << "  \"recall_at_100\": " << recall_100 << ",\n";
        jf << "  \"latency_avg_ms\": " << avg_lat << ",\n";
        jf << "  \"latency_p50_ms\": " << p50 << ",\n";
        jf << "  \"latency_p95_ms\": " << p95 << ",\n";
        jf << "  \"latency_p99_ms\": " << p99 << ",\n";
        jf << "  \"avg_probed\": " << avg_probed << ",\n";
        jf << "  \"early_stop_rate\": " << early_stop_rate << ",\n";
        jf << "  \"safein_pct\": " << (total_classified > 0 ? 100.0 * total_safein / total_classified : 0) << ",\n";
        jf << "  \"safeout_pct\": " << (total_classified > 0 ? 100.0 * total_safeout / total_classified : 0) << ",\n";
        jf << "  \"uncertain_pct\": " << (total_classified > 0 ? 100.0 * total_uncertain / total_classified : 0) << "\n";
        jf << "}\n";
        jf.close();
        Log("\nJSON results written to: %s\n", json_path.c_str());
    }

    Log("\nDone.\n");
    return 0;
}
