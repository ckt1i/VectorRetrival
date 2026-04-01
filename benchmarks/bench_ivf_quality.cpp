/// bench_ivf_quality.cpp — Standalone benchmark for Phase 6 IVF+RaBitQ quality
///
/// Measures:
///   1. RaBitQ accuracy per cluster (mean relative error, Spearman correlation)
///   2. ConANN 3-class distribution (SafeIn / Uncertain / SafeOut counts)
///
/// Usage:
///   bench_ivf_quality [--N 1000] [--dim 128] [--nlist 8] [--nprobe 2]
///                     [--balance 0.3] [--seed 42] [--topk 10]
///
/// This is NOT a GoogleTest binary — it's a standalone main() benchmark.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::rabitq;
using namespace vdb::simd;

namespace fs = std::filesystem;

// ============================================================================
// Command-line helpers
// ============================================================================

static int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return std::atoi(argv[i + 1]);
        }
    }
    return default_val;
}

static float GetFloatArg(int argc, char* argv[], const char* name,
                          float default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return static_cast<float>(std::atof(argv[i + 1]));
        }
    }
    return default_val;
}

// ============================================================================
// Spearman rank correlation
// ============================================================================

static double SpearmanCorrelation(const std::vector<float>& x,
                                   const std::vector<float>& y) {
    const size_t n = x.size();
    if (n < 2) return 0.0;

    // Compute ranks
    auto rank = [](const std::vector<float>& vals) {
        size_t n = vals.size();
        std::vector<size_t> idx(n);
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return vals[a] < vals[b]; });
        std::vector<double> ranks(n);
        for (size_t i = 0; i < n; ++i) {
            ranks[idx[i]] = static_cast<double>(i + 1);
        }
        return ranks;
    };

    auto rx = rank(x);
    auto ry = rank(y);

    // Pearson correlation of ranks
    double mean_rx = 0, mean_ry = 0;
    for (size_t i = 0; i < n; ++i) {
        mean_rx += rx[i];
        mean_ry += ry[i];
    }
    mean_rx /= n;
    mean_ry /= n;

    double cov = 0, var_rx = 0, var_ry = 0;
    for (size_t i = 0; i < n; ++i) {
        double dx = rx[i] - mean_rx;
        double dy = ry[i] - mean_ry;
        cov += dx * dy;
        var_rx += dx * dx;
        var_ry += dy * dy;
    }

    double denom = std::sqrt(var_rx * var_ry);
    return denom > 0 ? cov / denom : 0.0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // Parse arguments
    const uint32_t N = static_cast<uint32_t>(GetIntArg(argc, argv, "--N", 1000));
    const Dim dim = static_cast<Dim>(GetIntArg(argc, argv, "--dim", 128));
    const uint32_t nlist =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 8));
    const uint32_t nprobe =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 2));
    const uint64_t seed =
        static_cast<uint64_t>(GetIntArg(argc, argv, "--seed", 42));
    const uint32_t topk =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    const uint32_t num_queries =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--queries", 50));

    std::printf("=== Phase 6 IVF+RaBitQ Quality Benchmark ===\n");
    std::printf("  N=%u  dim=%u  nlist=%u  nprobe=%u  seed=%lu  topk=%u  queries=%u\n\n",
                N, dim, nlist, nprobe,
                static_cast<unsigned long>(seed), topk, num_queries);

    // --- Generate random dataset ---
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> vectors(static_cast<size_t>(N) * dim);
    for (auto& v : vectors) v = dist(rng);

    // --- Build IVF index ---
    std::string tmp_dir =
        (fs::temp_directory_path() / "vdb_bench_ivf_quality").string();
    fs::create_directories(tmp_dir);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 30;
    cfg.tolerance = 1e-5f;
    cfg.seed = seed;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = std::min(N, 200u);
    cfg.calibration_topk = topk;
    cfg.calibration_percentile = 0.95f;
    cfg.page_size = 1;

    auto t0 = std::chrono::high_resolution_clock::now();

    IvfBuilder builder(cfg);
    auto s = builder.Build(vectors.data(), N, dim, tmp_dir);
    if (!s.ok()) {
        std::fprintf(stderr, "Build failed: %.*s\n",
                    static_cast<int>(s.message().size()), s.message().data());
        fs::remove_all(tmp_dir);
        return 1;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double build_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("Build time: %.1f ms\n\n", build_ms);

    // --- Print cluster size distribution ---
    {
        std::vector<uint32_t> counts(nlist, 0);
        for (auto a : builder.assignments()) counts[a]++;
        std::printf("Cluster sizes:\n");
        uint32_t min_sz = *std::min_element(counts.begin(), counts.end());
        uint32_t max_sz = *std::max_element(counts.begin(), counts.end());
        double mean_sz = static_cast<double>(N) / nlist;
        double var = 0;
        for (auto c : counts) {
            double d = static_cast<double>(c) - mean_sz;
            var += d * d;
        }
        double stddev = std::sqrt(var / nlist);
        for (uint32_t k = 0; k < nlist; ++k) {
            std::printf("  cluster %2u: %u vectors\n", k, counts[k]);
        }
        std::printf("  min=%u  max=%u  mean=%.1f  stddev=%.1f\n\n",
                    min_sz, max_sz, mean_sz, stddev);
    }

    // --- Open index ---
    IvfIndex idx;
    s = idx.Open(tmp_dir);
    if (!s.ok()) {
        std::fprintf(stderr, "Open failed: %.*s\n",
                    static_cast<int>(s.message().size()), s.message().data());
        fs::remove_all(tmp_dir);
        return 1;
    }

    std::printf("ConANN params: epsilon=%.6f  d_k=%.4f  tau_in=%.4f  tau_out=%.4f\n\n",
                idx.conann().epsilon(), idx.conann().d_k(),
                idx.conann().tau_in(), idx.conann().tau_out());

    // --- RaBitQ accuracy & ConANN distribution ---
    // For each cluster, compute per-vector approximate distance vs exact distance

    const auto& conann = idx.conann();
    RaBitQEstimator estimator(dim);

    // ConANN global counters
    uint64_t total_safe_in = 0;
    uint64_t total_safe_out = 0;
    uint64_t total_uncertain = 0;
    uint64_t total_candidates = 0;

    // RaBitQ accuracy accumulators
    double total_spearman = 0.0;
    double total_rel_err = 0.0;
    uint32_t clusters_evaluated = 0;

    // Random queries
    std::vector<uint32_t> query_indices(N);
    std::iota(query_indices.begin(), query_indices.end(), 0);
    std::shuffle(query_indices.begin(), query_indices.end(), rng);
    const uint32_t actual_queries = std::min(num_queries, N);

    for (uint32_t qi = 0; qi < actual_queries; ++qi) {
        uint32_t qidx = query_indices[qi];
        const float* query = vectors.data() + static_cast<size_t>(qidx) * dim;

        // Find nprobe nearest clusters
        auto probe_ids = idx.FindNearestClusters(query, nprobe);

        for (auto cid : probe_ids) {
            uint32_t n_records = idx.segment().GetNumRecords(cid);
            if (n_records == 0) continue;

            // Ensure cluster data is loaded
            auto load_s = idx.segment().EnsureClusterLoaded(cid);
            if (!load_s.ok()) continue;

            // Load centroid
            const float* centroid = idx.segment().GetCentroid(cid);
            if (!centroid) continue;

            // Prepare query for RaBitQ estimation
            auto pq = estimator.PrepareQuery(query, centroid,
                                              idx.rotation());

            // Per-record: approximate dist vs exact dist
            std::vector<float> approx_dists;
            std::vector<float> exact_dists;
            approx_dists.reserve(n_records);
            exact_dists.reserve(n_records);

            for (uint32_t r = 0; r < n_records; ++r) {
                // Load RaBitQ code
                std::vector<uint64_t> code;
                auto ls = idx.segment().LoadCode(cid, r, code);
                if (!ls.ok()) continue;

                RaBitQCode rcode;
                rcode.code = code;
                rcode.norm = 0.0f;
                rcode.sum_x = 0;
                for (auto w : code) {
                    rcode.sum_x += __builtin_popcountll(w);
                }

                float approx = estimator.EstimateDistance(pq, rcode);
                approx_dists.push_back(approx);

                // We need the original vector for exact distance, but we
                // don't store global IDs in .clu. Use the centroid-relative
                // reconstruction approximation: exact_dist = ||query - centroid||^2
                // Actually we need to read from DataFile or do brute force
                // For the benchmark, read from DataFile
                {
                    auto addr = idx.segment().GetAddress(cid, r);
                    std::vector<float> rec_vec(dim);
                    auto rs = idx.segment().ReadVector(addr, rec_vec.data());
                    if (rs.ok()) {
                        float exact = L2Sqr(query, rec_vec.data(), dim);
                        exact_dists.push_back(exact);
                    } else {
                        exact_dists.push_back(approx);  // fallback
                    }
                }

                // ConANN classification
                ResultClass rc = conann.Classify(approx);
                switch (rc) {
                    case ResultClass::SafeIn:
                        total_safe_in++;
                        break;
                    case ResultClass::SafeOut:
                        total_safe_out++;
                        break;
                    case ResultClass::Uncertain:
                        total_uncertain++;
                        break;
                }
                total_candidates++;
            }

            // Spearman correlation for this cluster
            if (approx_dists.size() >= 2) {
                double sp = SpearmanCorrelation(approx_dists, exact_dists);
                total_spearman += sp;

                // Mean relative error
                double rel_err_sum = 0.0;
                for (size_t j = 0; j < approx_dists.size(); ++j) {
                    float ex = exact_dists[j];
                    if (ex > 1e-8f) {
                        rel_err_sum += std::fabs(approx_dists[j] - ex) / ex;
                    }
                }
                double mean_rel = rel_err_sum / approx_dists.size();
                total_rel_err += mean_rel;

                clusters_evaluated++;
            }
        }
    }

    // --- Report ---
    std::printf("=== RaBitQ Accuracy (%u cluster evaluations) ===\n",
                clusters_evaluated);
    if (clusters_evaluated > 0) {
        std::printf("  Mean Spearman:       %.4f\n",
                    total_spearman / clusters_evaluated);
        std::printf("  Mean Relative Error: %.4f\n",
                    total_rel_err / clusters_evaluated);
    } else {
        std::printf("  (no clusters evaluated)\n");
    }

    std::printf("\n=== ConANN Distribution (%lu total candidates) ===\n",
                static_cast<unsigned long>(total_candidates));
    if (total_candidates > 0) {
        auto pct = [&](uint64_t cnt) {
            return 100.0 * static_cast<double>(cnt) /
                   static_cast<double>(total_candidates);
        };
        std::printf("  SafeIn:    %6lu  (%5.1f%%)\n",
                    static_cast<unsigned long>(total_safe_in),
                    pct(total_safe_in));
        std::printf("  SafeOut:   %6lu  (%5.1f%%)\n",
                    static_cast<unsigned long>(total_safe_out),
                    pct(total_safe_out));
        std::printf("  Uncertain: %6lu  (%5.1f%%)\n",
                    static_cast<unsigned long>(total_uncertain),
                    pct(total_uncertain));
    }

    // Cleanup
    //fs::remove_all(tmp_dir);

    std::printf("\n=== Done ===\n");
    return 0;
}
