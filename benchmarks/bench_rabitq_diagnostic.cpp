/// bench_rabitq_diagnostic.cpp — Diagnostic benchmark for RaBitQ distance
/// estimation and ConANN classification.
///
/// Phase 1: Compare RaBitQ estimated distances vs exact L2, track kth-distance
///          convergence curves and top-K overlap as clusters are probed.
/// Phase 2: Evaluate ConANN Classify/ClassifyAdaptive accuracy under RaBitQ
///          estimates, including confusion matrices and false SafeOut rates.
///
/// Outputs CSV files for Python visualization.
///
/// Usage:
///   bench_rabitq_diagnostic [--dataset path] [--nlist 32] [--queries 100]
///       [--topk 10] [--outdir ./diag_output] [--phase 0]
///       [--scatter-queries 10]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "superkmeans/superkmeans.h"

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/io/npy_reader.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/popcount.h"

using namespace vdb;
using namespace vdb::rabitq;
using namespace vdb::index;

// ============================================================================
// Helpers
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

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static void RunSuperKMeans(const float* data, uint32_t N, Dim dim, uint32_t K,
                           std::vector<float>& centroids,
                           std::vector<uint32_t>& assignments) {
    skmeans::SuperKMeansConfig cfg;
    cfg.iters = 10;
    cfg.seed = 42;
    cfg.verbose = false;
    auto skm = skmeans::SuperKMeans(K, dim, cfg);
    auto c = skm.Train(data, N);
    auto a = skm.Assign(data, c.data(), N, K);
    centroids.assign(c.begin(), c.end());
    assignments.assign(a.begin(), a.end());
}

static const char* ClassToStr(ResultClass rc) {
    switch (rc) {
        case ResultClass::SafeIn:    return "SafeIn";
        case ResultClass::SafeOut:   return "SafeOut";
        case ResultClass::Uncertain: return "Uncertain";
    }
    return "?";
}

static int ClassToIdx(ResultClass rc) {
    switch (rc) {
        case ResultClass::SafeIn:    return 0;
        case ResultClass::SafeOut:   return 1;
        case ResultClass::Uncertain: return 2;
    }
    return 2;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetArg(argc, argv, "--dataset",
                                  "/home/zcq/VDB/data/coco_1k");
    std::string base_path = GetArg(argc, argv, "--base", "");
    std::string query_path = GetArg(argc, argv, "--query", "");
    uint32_t nlist = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 32));
    int q_limit = GetIntArg(argc, argv, "--queries", 100);
    uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    int phase = GetIntArg(argc, argv, "--phase", 0);
    std::string outdir = GetArg(argc, argv, "--outdir", "./diag_output");
    uint32_t scatter_queries = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--scatter-queries", 10));
    uint32_t p_for_dk = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--p-for-dk", 90));
    uint32_t p_for_eps = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--p-for-eps", 95));
    uint32_t samples_for_eps = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--samples-for-eps", 100));

    // Fallback: --base/--query not specified → derive from --dataset
    if (base_path.empty())  base_path  = data_dir + "/image_embeddings.npy";
    if (query_path.empty()) query_path = data_dir + "/query_embeddings.npy";

    bool run_phase1 = (phase == 0 || phase == 1);
    bool run_phase2 = (phase == 0 || phase == 2);

    Log("=== RaBitQ Diagnostic Benchmark ===\n");
    Log("Base:     %s\n", base_path.c_str());
    Log("Query:    %s\n", query_path.c_str());
    Log("Outdir:   %s\n", outdir.c_str());
    Log("Phase:    %d (%s%s%s)\n", phase,
        run_phase1 ? "dist" : "", (run_phase1 && run_phase2) ? "+" : "",
        run_phase2 ? "classify" : "");

    // Create output directory
    std::string mkdir_cmd = "mkdir -p " + outdir;
    int rc = std::system(mkdir_cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "Failed to create output directory: %s\n", outdir.c_str());
        return 1;
    }

    // ================================================================
    // Phase 0: Load data + KMeans + RaBitQ encode + calibrate
    // ================================================================
    Log("\n[Phase 0] Loading data...\n");

    auto img_or = io::LoadVectors(base_path);
    if (!img_or.ok()) {
        std::fprintf(stderr, "Failed to load base: %s\n", img_or.status().ToString().c_str());
        return 1;
    }
    auto& img = img_or.value();
    uint32_t N = img.rows;
    Dim dim = static_cast<Dim>(img.cols);

    auto qry_or = io::LoadVectors(query_path);
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load query: %s\n", qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q = (q_limit > 0) ? std::min(static_cast<uint32_t>(q_limit), qry.rows)
                                : qry.rows;

    Log("  N=%u, Q=%u, dim=%u, nlist=%u, top_k=%u\n", N, Q, dim, nlist, top_k);

    // KMeans
    Log("[Phase 0] K-Means (K=%u) + RaBitQ encoding...\n", nlist);
    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    RunSuperKMeans(img.data.data(), N, dim, nlist, centroids, assignments);

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);
    RaBitQEstimator estimator(dim);

    std::vector<RaBitQCode> codes(N);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t k = assignments[i];
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        codes[i] = encoder.Encode(
            img.data.data() + static_cast<size_t>(i) * dim, centroid);
    }
    Log("  Encoded %u vectors.\n", N);

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

    // Global ε_ip calibration
    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    const uint32_t num_words = (dim + 63) / 64;

    std::vector<float> ip_errors;
    for (uint32_t k = 0; k < nlist; ++k) {
        const auto& members = cluster_members[k];
        if (members.size() < 2) continue;
        uint32_t n_queries = std::min(samples_for_eps,
                                       static_cast<uint32_t>(members.size()));
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        std::vector<uint32_t> indices(members.size());
        std::iota(indices.begin(), indices.end(), 0u);
        std::mt19937 rng(42 + k);
        std::shuffle(indices.begin(), indices.end(), rng);

        for (uint32_t q = 0; q < n_queries; ++q) {
            uint32_t q_global = members[indices[q]];
            const float* q_vec = img.data.data() +
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
    Log("  ε_ip = %.6f (P%u)\n", eps_ip, p_for_eps);

    // d_k calibration
    float p_dk_f = static_cast<float>(p_for_dk) / 100.0f;
    float d_k = ConANN::CalibrateDistanceThreshold(
        qry.data.data(), Q, img.data.data(), N,
        dim, 100, top_k, p_dk_f, 42);
    Log("  d_k = %.4f (P%u)\n", d_k, p_for_dk);

    // Brute-force exact GT
    Log("[Phase 0] Computing exact ground truth...\n");
    std::vector<std::unordered_set<uint32_t>> gt_sets(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
        std::vector<std::pair<float, uint32_t>> dists(N);
        for (uint32_t i = 0; i < N; ++i) {
            dists[i] = {simd::L2Sqr(q_vec,
                img.data.data() + static_cast<size_t>(i) * dim, dim), i};
        }
        std::partial_sort(dists.begin(), dists.begin() + top_k, dists.end());
        for (uint32_t i = 0; i < top_k; ++i) {
            gt_sets[qi].insert(dists[i].second);
        }
    }
    Log("  GT computed for %u queries.\n\n", Q);

    // ================================================================
    // Phase 1 + 2: Fused probing loop
    // ================================================================
    // We fuse Phase 1 and Phase 2 into one loop since both need the same
    // per-query per-cluster per-vector distances.

    Log("[Phase 1+2] Running diagnostic probing (%u queries × %u clusters)...\n",
        Q, nlist);

    auto t0 = std::chrono::steady_clock::now();

    // CSV file handles
    std::ofstream csv_kth, csv_scatter, csv_classify;

    if (run_phase1) {
        csv_kth.open(outdir + "/kth_convergence.csv");
        csv_kth << "query_id,probed_count,exact_kth,est_kth,topk_overlap\n";

        csv_scatter.open(outdir + "/vector_distances.csv");
        csv_scatter << "query_id,vector_id,cluster_id,exact_dist,est_dist,is_true_topk\n";
    }
    if (run_phase2) {
        csv_classify.open(outdir + "/classification.csv");
        csv_classify << "query_id,vector_id,cluster_id,probe_step,"
                     << "exact_dist,est_dist,margin,"
                     << "class_exact,class_est,class_est_adaptive,is_true_topk\n";
    }

    // Phase 1 accumulators
    float exact_d_min = std::numeric_limits<float>::infinity();
    float exact_d_max = -std::numeric_limits<float>::infinity();
    float est_d_min = std::numeric_limits<float>::infinity();
    float est_d_max = -std::numeric_limits<float>::infinity();
    std::vector<double> sum_overlap_per_step(nlist, 0.0);

    // Phase 2 accumulators: confusion[row=exact_class][col=est_class]
    uint64_t confusion_est[3][3] = {};       // exact vs Classify(est)
    uint64_t confusion_adaptive[3][3] = {};  // exact vs ClassifyAdaptive(est)
    uint64_t total_true_nn = 0;
    uint64_t false_safeout_est = 0;       // true NN classified SafeOut by est
    uint64_t false_safeout_adaptive = 0;  // true NN classified SafeOut by adaptive
    uint64_t total_flips_est = 0;
    uint64_t total_flips_adaptive = 0;
    uint64_t total_vectors_classified = 0;
    // Collect all margins for quartile boundaries
    std::vector<float> all_margins;
    if (run_phase2) {
        // Pre-estimate total vectors for reserve
        all_margins.reserve(static_cast<size_t>(Q) * N / 4);  // rough estimate
    }

    ConANN conann(eps_ip, d_k);

    for (uint32_t qi = 0; qi < Q; ++qi) {
        if (qi % 20 == 0) {
            Log("  query %u/%u\n", qi, Q);
        }

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
        std::vector<std::pair<float, uint32_t>> exact_heap;
        std::vector<std::pair<float, uint32_t>> est_heap;

        // Probe clusters in order
        for (uint32_t p = 0; p < nlist; ++p) {
            uint32_t cid = centroid_dists[p].second;
            const float* centroid = centroids.data() +
                static_cast<size_t>(cid) * dim;
            auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            float margin = 2.0f * per_cluster_r_max[cid] * pq.norm_qc * eps_ip;

            // est_kth at the start of this cluster (for ClassifyAdaptive)
            float est_kth_before = (est_heap.size() >= top_k)
                ? est_heap.front().first
                : std::numeric_limits<float>::infinity();

            for (uint32_t vi = 0; vi < cluster_members[cid].size(); ++vi) {
                uint32_t vid = cluster_members[cid][vi];
                const float* vec = img.data.data() +
                    static_cast<size_t>(vid) * dim;

                float exact_dist = simd::L2Sqr(q_vec, vec, dim);
                float est_dist = estimator.EstimateDistance(pq, codes[vid]);
                bool is_nn = gt_sets[qi].count(vid) > 0;

                // Update exact heap
                if (exact_heap.size() < top_k) {
                    exact_heap.push_back({exact_dist, vid});
                    std::push_heap(exact_heap.begin(), exact_heap.end());
                } else if (exact_dist < exact_heap.front().first) {
                    std::pop_heap(exact_heap.begin(), exact_heap.end());
                    exact_heap.back() = {exact_dist, vid};
                    std::push_heap(exact_heap.begin(), exact_heap.end());
                }

                // Update est heap
                if (est_heap.size() < top_k) {
                    est_heap.push_back({est_dist, vid});
                    std::push_heap(est_heap.begin(), est_heap.end());
                } else if (est_dist < est_heap.front().first) {
                    std::pop_heap(est_heap.begin(), est_heap.end());
                    est_heap.back() = {est_dist, vid};
                    std::push_heap(est_heap.begin(), est_heap.end());
                }

                // Phase 1: scatter CSV (sampled queries)
                if (run_phase1 && qi < scatter_queries) {
                    csv_scatter << qi << "," << vid << "," << cid << ","
                                << exact_dist << "," << est_dist << ","
                                << (is_nn ? 1 : 0) << "\n";
                }

                // Phase 2: classification
                if (run_phase2) {
                    ResultClass class_exact = conann.Classify(exact_dist, margin);
                    ResultClass class_est = conann.Classify(est_dist, margin);
                    ResultClass class_adaptive = conann.ClassifyAdaptive(
                        est_dist, margin, est_kth_before);

                    csv_classify << qi << "," << vid << "," << cid << ","
                                 << (p + 1) << ","
                                 << exact_dist << "," << est_dist << ","
                                 << margin << ","
                                 << ClassToStr(class_exact) << ","
                                 << ClassToStr(class_est) << ","
                                 << ClassToStr(class_adaptive) << ","
                                 << (is_nn ? 1 : 0) << "\n";

                    int ei = ClassToIdx(class_exact);
                    int si = ClassToIdx(class_est);
                    int ai = ClassToIdx(class_adaptive);
                    confusion_est[ei][si]++;
                    confusion_adaptive[ei][ai]++;
                    total_vectors_classified++;

                    if (ei != si) total_flips_est++;
                    if (ei != ai) total_flips_adaptive++;

                    if (is_nn) {
                        total_true_nn++;
                        if (class_est == ResultClass::SafeOut) false_safeout_est++;
                        if (class_adaptive == ResultClass::SafeOut) false_safeout_adaptive++;
                    }

                    all_margins.push_back(margin);
                }
            }

            // Phase 1: record kth convergence after this cluster
            if (run_phase1) {
                float exact_kth = (exact_heap.size() >= top_k)
                    ? exact_heap.front().first
                    : std::numeric_limits<float>::infinity();
                float est_kth = (est_heap.size() >= top_k)
                    ? est_heap.front().first
                    : std::numeric_limits<float>::infinity();

                // Top-K overlap
                std::unordered_set<uint32_t> exact_ids, est_ids;
                for (auto& [d, id] : exact_heap) exact_ids.insert(id);
                for (auto& [d, id] : est_heap) est_ids.insert(id);
                uint32_t overlap = 0;
                for (uint32_t id : exact_ids) {
                    if (est_ids.count(id)) overlap++;
                }

                csv_kth << qi << "," << (p + 1) << ","
                        << exact_kth << "," << est_kth << ","
                        << overlap << "\n";

                // Accumulate d_min/d_max
                if (std::isfinite(exact_kth)) {
                    exact_d_min = std::min(exact_d_min, exact_kth);
                    exact_d_max = std::max(exact_d_max, exact_kth);
                }
                if (std::isfinite(est_kth)) {
                    est_d_min = std::min(est_d_min, est_kth);
                    est_d_max = std::max(est_d_max, est_kth);
                }
                sum_overlap_per_step[p] += overlap;
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    Log("\n  Probing complete (%.1f ms).\n\n", elapsed_ms);

    // Close CSV files
    if (csv_kth.is_open()) csv_kth.close();
    if (csv_scatter.is_open()) csv_scatter.close();
    if (csv_classify.is_open()) csv_classify.close();

    // ================================================================
    // Phase 1 summary
    // ================================================================
    if (run_phase1) {
        Log("=== Phase 1: Distance Distribution Summary ===\n\n");
        Log("  d_min/d_max comparison:\n");
        Log("  %-14s %-14s %-14s %-14s\n", "", "d_min", "d_max", "range");
        Log("  %-14s %-14s %-14s %-14s\n", "------", "------", "------", "------");
        Log("  %-14s %-14.4f %-14.4f %-14.4f\n", "Exact L2",
            exact_d_min, exact_d_max, exact_d_max - exact_d_min);
        Log("  %-14s %-14.4f %-14.4f %-14.4f\n", "RaBitQ Est",
            est_d_min, est_d_max, est_d_max - est_d_min);
        Log("  %-14s %-14.2fx\n", "Range ratio",
            (exact_d_max - exact_d_min) > 0
                ? (est_d_max - est_d_min) / (exact_d_max - exact_d_min)
                : 0.0f);
        Log("\n");

        Log("  Mean top-K overlap per step (last 5):\n");
        for (uint32_t p = (nlist > 5 ? nlist - 5 : 0); p < nlist; ++p) {
            Log("    step %2u: %.2f / %u\n", p + 1,
                sum_overlap_per_step[p] / Q, top_k);
        }
        Log("\n  CSV files written:\n");
        Log("    %s/kth_convergence.csv\n", outdir.c_str());
        Log("    %s/vector_distances.csv\n", outdir.c_str());
        Log("\n");
    }

    // ================================================================
    // Phase 2 summary
    // ================================================================
    if (run_phase2) {
        Log("=== Phase 2: ConANN Classification Summary ===\n\n");

        const char* class_names[3] = {"SafeIn", "SafeOut", "Uncertain"};

        // Confusion matrix: exact vs Classify(est)
        Log("  Confusion: Exact class (rows) vs Classify(est_dist) (cols)\n");
        Log("  %-12s %-12s %-12s %-12s\n", "", "est_SI", "est_SO", "est_Unc");
        for (int r = 0; r < 3; ++r) {
            Log("  %-12s %-12lu %-12lu %-12lu\n", class_names[r],
                confusion_est[r][0], confusion_est[r][1], confusion_est[r][2]);
        }
        Log("\n");

        // Confusion matrix: exact vs ClassifyAdaptive(est)
        Log("  Confusion: Exact class (rows) vs ClassifyAdaptive(est_dist) (cols)\n");
        Log("  %-12s %-12s %-12s %-12s\n", "", "adp_SI", "adp_SO", "adp_Unc");
        for (int r = 0; r < 3; ++r) {
            Log("  %-12s %-12lu %-12lu %-12lu\n", class_names[r],
                confusion_adaptive[r][0], confusion_adaptive[r][1],
                confusion_adaptive[r][2]);
        }
        Log("\n");

        // False SafeOut rates
        Log("  False SafeOut (true NN misclassified as SafeOut):\n");
        Log("    Classify(est):         %lu / %lu (%.4f%%)\n",
            false_safeout_est, total_true_nn,
            total_true_nn > 0 ? 100.0 * static_cast<double>(false_safeout_est) /
                                    static_cast<double>(total_true_nn) : 0.0);
        Log("    ClassifyAdaptive(est): %lu / %lu (%.4f%%)\n",
            false_safeout_adaptive, total_true_nn,
            total_true_nn > 0 ? 100.0 * static_cast<double>(false_safeout_adaptive) /
                                    static_cast<double>(total_true_nn) : 0.0);
        Log("\n");

        // Classification flip rates
        Log("  Classification flip rate (exact_class != est_class):\n");
        Log("    Classify:              %lu / %lu (%.2f%%)\n",
            total_flips_est, total_vectors_classified,
            total_vectors_classified > 0
                ? 100.0 * static_cast<double>(total_flips_est) /
                      static_cast<double>(total_vectors_classified)
                : 0.0);
        Log("    ClassifyAdaptive:      %lu / %lu (%.2f%%)\n",
            total_flips_adaptive, total_vectors_classified,
            total_vectors_classified > 0
                ? 100.0 * static_cast<double>(total_flips_adaptive) /
                      static_cast<double>(total_vectors_classified)
                : 0.0);
        Log("\n");

        // SafeOut % comparison
        uint64_t est_so_total = confusion_est[0][1] + confusion_est[1][1] + confusion_est[2][1];
        uint64_t adp_so_total = confusion_adaptive[0][1] + confusion_adaptive[1][1] + confusion_adaptive[2][1];
        // exact SafeOut total is the row sum
        uint64_t exact_so_total = confusion_est[1][0] + confusion_est[1][1] + confusion_est[1][2];

        Log("  SafeOut %% comparison:\n");
        Log("    Classify(exact):       %.2f%%\n",
            total_vectors_classified > 0
                ? 100.0 * static_cast<double>(exact_so_total) /
                      static_cast<double>(total_vectors_classified)
                : 0.0);
        Log("    Classify(est):         %.2f%%\n",
            total_vectors_classified > 0
                ? 100.0 * static_cast<double>(est_so_total) /
                      static_cast<double>(total_vectors_classified)
                : 0.0);
        Log("    ClassifyAdaptive(est): %.2f%%\n",
            total_vectors_classified > 0
                ? 100.0 * static_cast<double>(adp_so_total) /
                      static_cast<double>(total_vectors_classified)
                : 0.0);
        Log("\n");

        // Margin quartile flip analysis
        if (!all_margins.empty()) {
            std::vector<float> sorted_margins = all_margins;
            std::sort(sorted_margins.begin(), sorted_margins.end());
            float q1 = sorted_margins[sorted_margins.size() / 4];
            float q2 = sorted_margins[sorted_margins.size() / 2];
            float q3 = sorted_margins[sorted_margins.size() * 3 / 4];

            // Re-read classification CSV to compute per-quartile stats
            // Instead, we note that we'd need to re-scan. For simplicity,
            // we output the quartile boundaries and let Python do the analysis.
            Log("  Margin quartile boundaries: Q1=%.4f Q2=%.4f Q3=%.4f\n",
                q1, q2, q3);
            Log("  (Detailed per-quartile flip analysis in Python plots)\n");
        }

        Log("\n  CSV files written:\n");
        Log("    %s/classification.csv\n", outdir.c_str());
        Log("\n");
    }

    Log("Done.\n");
    return 0;
}
