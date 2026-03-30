/// bench_conann_crc.cpp — ConANN CRC (Conformal Risk Control) benchmark.
///
/// Validates the CRC framework for adaptive nprobe early stopping
/// using exact L2 distances (no RaBitQ).
///
/// Usage:
///   bench_conann_crc [--dataset path] [--nlist 16] [--topk 10]
///                    [--alpha 0.1] [--queries 0] [--sweep-alpha]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/io/npy_reader.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;

// ============================================================================
// Helpers (reused from bench_conann_recall)
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

static bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// K-Means (identical to bench_conann_recall)
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
// Recall computation
// ============================================================================

static double ComputeRecallAtK(const std::vector<uint32_t>& predicted,
                                const std::vector<uint32_t>& gt,
                                uint32_t K) {
    uint32_t pk = std::min(K, static_cast<uint32_t>(predicted.size()));
    uint32_t gk = std::min(K, static_cast<uint32_t>(gt.size()));
    std::unordered_set<uint32_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return gk > 0 ? static_cast<double>(hits) / gk : 0.0;
}

// ============================================================================
// Run CRC for a given alpha (calibrate + search + evaluate)
// ============================================================================

struct CrcBenchResult {
    CalibrationResults cal;
    CrcCalibrator::EvalResults eval;
    // Full-query-set search results (using CrcStopper directly).
    double search_recall_at_1 = 0;
    double search_recall_at_5 = 0;
    double search_recall_at_10 = 0;
    double search_avg_probed = 0;
    double search_fnr = 0;
    uint32_t search_early_stopped = 0;
};

static CrcBenchResult RunCrc(
    float alpha, uint32_t top_k, uint32_t nlist, uint32_t max_nprobe,
    Dim dim, uint64_t seed,
    const float* img_data,
    const float* qry_data, uint32_t Q,
    const std::vector<float>& centroids,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const std::vector<std::vector<uint32_t>>& gt_topk) {

    CrcBenchResult result;

    // Build ClusterData for calibrator.
    std::vector<std::vector<float>> cluster_vecs(nlist);
    std::vector<ClusterData> clusters(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        cluster_vecs[c].resize(static_cast<size_t>(cluster_members[c].size()) * dim);
        for (size_t vi = 0; vi < cluster_members[c].size(); ++vi) {
            uint32_t gid = cluster_members[c][vi];
            std::memcpy(cluster_vecs[c].data() + vi * dim,
                        img_data + static_cast<size_t>(gid) * dim,
                        dim * sizeof(float));
        }
        clusters[c].vectors = cluster_vecs[c].data();
        clusters[c].ids = cluster_members[c].data();
        clusters[c].count = static_cast<uint32_t>(cluster_members[c].size());
    }

    // Calibrate.
    CrcCalibrator::Config config;
    config.alpha = alpha;
    config.top_k = top_k;
    config.seed = seed;

    auto [cal, eval] = CrcCalibrator::Calibrate(
        config, qry_data, Q, dim,
        centroids.data(), nlist, clusters);

    result.cal = cal;
    result.eval = eval;

    // Full search with CrcStopper.
    CrcStopper stopper(cal, nlist);

    double sum_r1 = 0, sum_r5 = 0, sum_r10 = 0, sum_probed = 0;
    uint32_t fn_count = 0, early_count = 0;

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry_data + static_cast<size_t>(qi) * dim;

        // Sort clusters by centroid distance.
        std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
        for (uint32_t c = 0; c < nlist; ++c) {
            centroid_dists[c] = {
                simd::L2Sqr(qvec, centroids.data() + static_cast<size_t>(c) * dim, dim), c};
        }
        std::sort(centroid_dists.begin(), centroid_dists.end());

        // Incremental search with CRC early stop.
        std::vector<std::pair<float, uint32_t>> topk_heap;
        uint32_t probed = 0;
        bool stopped = false;

        for (uint32_t p = 0; p < max_nprobe; ++p) {
            uint32_t cid = centroid_dists[p].second;
            for (uint32_t vi = 0; vi < cluster_members[cid].size(); ++vi) {
                uint32_t gid = cluster_members[cid][vi];
                float dist = simd::L2Sqr(qvec,
                    img_data + static_cast<size_t>(gid) * dim, dim);
                if (topk_heap.size() < top_k) {
                    topk_heap.push_back({dist, gid});
                    std::push_heap(topk_heap.begin(), topk_heap.end());
                } else if (dist < topk_heap.front().first) {
                    std::pop_heap(topk_heap.begin(), topk_heap.end());
                    topk_heap.back() = {dist, gid};
                    std::push_heap(topk_heap.begin(), topk_heap.end());
                }
            }
            probed++;

            float kth_dist = topk_heap.empty()
                ? std::numeric_limits<float>::infinity()
                : topk_heap.front().first;
            if (stopper.ShouldStop(probed, kth_dist)) {
                stopped = true;
                break;
            }
        }

        // Extract results.
        std::sort_heap(topk_heap.begin(), topk_heap.end());
        std::vector<uint32_t> result_ids;
        result_ids.reserve(topk_heap.size());
        for (auto& [d, id] : topk_heap) result_ids.push_back(id);

        sum_r1 += ComputeRecallAtK(result_ids, gt_topk[qi], 1);
        sum_r5 += ComputeRecallAtK(result_ids, gt_topk[qi], 5);
        sum_r10 += ComputeRecallAtK(result_ids, gt_topk[qi],
                                     std::min(10u, top_k));
        sum_probed += probed;
        if (stopped) early_count++;

        // Check FNR: any GT ID missing from result?
        std::unordered_set<uint32_t> pred_set(result_ids.begin(), result_ids.end());
        for (uint32_t gt_id : gt_topk[qi]) {
            if (!pred_set.count(gt_id)) { fn_count++; break; }
        }
    }

    result.search_recall_at_1 = sum_r1 / Q;
    result.search_recall_at_5 = sum_r5 / Q;
    result.search_recall_at_10 = sum_r10 / Q;
    result.search_avg_probed = sum_probed / Q;
    result.search_fnr = static_cast<double>(fn_count) / Q;
    result.search_early_stopped = early_count;

    return result;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetArg(argc, argv, "--dataset",
                                  "/home/zcq/VDB/data/coco_1k");
    uint32_t nlist = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 16));
    uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    float alpha = GetFloatArg(argc, argv, "--alpha", 0.1f);
    int q_limit = GetIntArg(argc, argv, "--queries", 0);
    uint32_t max_nprobe = static_cast<uint32_t>(GetIntArg(argc, argv, "--max-nprobe", 0));
    uint32_t max_iter = static_cast<uint32_t>(GetIntArg(argc, argv, "--max-iter", 20));
    uint64_t seed = static_cast<uint64_t>(GetIntArg(argc, argv, "--seed", 42));
    bool sweep = HasFlag(argc, argv, "--sweep-alpha");

    Log("=== ConANN CRC Benchmark (Exact Distances) ===\n");
    Log("Dataset: %s\n", data_dir.c_str());
    Log("nlist=%u, topk=%u, alpha=%.2f\n\n", nlist, top_k, alpha);

    // ================================================================
    // Phase 1: Load data
    // ================================================================
    Log("[Phase 1] Loading data...\n");

    auto img_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_or.ok()) {
        std::fprintf(stderr, "Failed to load images: %s\n",
                     img_or.status().ToString().c_str());
        return 1;
    }
    auto& img = img_or.value();
    uint32_t N = img.rows;
    Dim dim = static_cast<Dim>(img.cols);

    auto qry_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load queries: %s\n",
                     qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q_total = qry.rows;
    uint32_t Q = (q_limit > 0 && static_cast<uint32_t>(q_limit) < Q_total)
                     ? static_cast<uint32_t>(q_limit) : Q_total;

    Log("  N=%u, Q=%u/%u, dim=%u\n\n", N, Q, Q_total, dim);

    // ================================================================
    // Phase 2: Brute-force ground truth
    // ================================================================
    Log("[Phase 2] Computing brute-force ground truth (top-%u)...\n", top_k);

    auto t_bf = std::chrono::steady_clock::now();
    std::vector<std::vector<uint32_t>> gt_topk(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry.data.data() + static_cast<size_t>(qi) * dim;
        std::vector<std::pair<float, uint32_t>> dists(N);
        for (uint32_t j = 0; j < N; ++j) {
            dists[j] = {simd::L2Sqr(qvec,
                img.data.data() + static_cast<size_t>(j) * dim, dim), j};
        }
        std::partial_sort(dists.begin(), dists.begin() + top_k, dists.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        gt_topk[qi].resize(top_k);
        for (uint32_t k = 0; k < top_k; ++k) {
            gt_topk[qi][k] = dists[k].second;
        }
    }
    double bf_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_bf).count();
    Log("  Brute-force time: %.1f ms\n\n", bf_ms);

    // ================================================================
    // Phase 3: K-Means clustering
    // ================================================================
    Log("[Phase 3] K-Means (K=%u, iter=%u)...\n", nlist, max_iter);

    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    KMeans(img.data.data(), N, dim, nlist, max_iter, seed, centroids, assignments);

    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    Log("  Cluster sizes:");
    for (uint32_t k = 0; k < nlist; ++k) {
        Log(" %zu", cluster_members[k].size());
    }
    Log("\n\n");

    // Clamp max_nprobe: 0 means no limit.
    if (max_nprobe == 0 || max_nprobe > nlist) max_nprobe = nlist;

    // ================================================================
    // Phase 4+5: CRC calibration + search
    // ================================================================

    auto print_result = [&](float a, const CrcBenchResult& r) {
        Log("  alpha=%.2f  |  lamhat=%.4f  kreg=%u  reg_lambda=%.4f\n",
            a, r.cal.lamhat, r.cal.kreg, r.cal.reg_lambda);
        Log("             |  d_min=%.4f  d_max=%.4f\n",
            r.cal.d_min, r.cal.d_max);
        Log("  Calibrator test set (n=%u):\n", r.eval.test_size);
        Log("    FNR: %.4f (target <= %.2f)\n", r.eval.actual_fnr, a);
        Log("    avg_probed: %.2f / %u\n", r.eval.avg_probed, nlist);
        Log("    recall@1=%.4f  recall@5=%.4f  recall@10=%.4f\n",
            r.eval.recall_at_1, r.eval.recall_at_5, r.eval.recall_at_10);
        Log("  CrcStopper full search (n=%u):\n", Q);
        Log("    FNR: %.4f\n", r.search_fnr);
        Log("    avg_probed: %.2f / %u\n", r.search_avg_probed, nlist);
        Log("    early_stopped: %u / %u (%.1f%%)\n",
            r.search_early_stopped, Q,
            100.0 * r.search_early_stopped / Q);
        Log("    recall@1=%.4f  recall@5=%.4f  recall@10=%.4f\n",
            r.search_recall_at_1, r.search_recall_at_5, r.search_recall_at_10);
    };

    if (!sweep) {
        Log("[Phase 4] CRC Calibration (alpha=%.2f)...\n", alpha);
        auto t_cal = std::chrono::steady_clock::now();
        auto res = RunCrc(alpha, top_k, nlist, max_nprobe, dim, seed,
                          img.data.data(), qry.data.data(), Q,
                          centroids, cluster_members, gt_topk);
        double cal_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_cal).count();
        Log("  Calibration + search time: %.1f ms\n\n", cal_ms);
        print_result(alpha, res);
    } else {
        Log("[Phase 4] Alpha Sweep...\n\n");
        float alphas[] = {0.01f, 0.05f, 0.1f, 0.15f, 0.2f, 0.3f};

        Log("%-7s | %-7s | %-10s | %-9s | %-9s | %-9s\n",
            "alpha", "FNR", "avg_probed", "recall@1", "recall@5", "recall@10");
        Log("--------|---------|------------|-----------|-----------|----------\n");

        for (float a : alphas) {
            auto res = RunCrc(a, top_k, nlist, max_nprobe, dim, seed,
                              img.data.data(), qry.data.data(), Q,
                              centroids, cluster_members, gt_topk);
            Log("%-7.2f | %-7.4f | %-10.2f | %-9.4f | %-9.4f | %-9.4f\n",
                a, res.search_fnr, res.search_avg_probed,
                res.search_recall_at_1, res.search_recall_at_5,
                res.search_recall_at_10);
        }
    }

    Log("\nDone.\n");
    return 0;
}
