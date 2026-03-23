/// bench_rabitq_accuracy.cpp — Measure RaBitQ 1-bit distance estimation accuracy.
///
/// For each query × each database vector:
///   - Compute exact L2² distance
///   - Compute RaBitQ estimated distance (with correct norm)
///   - Compare and report error statistics
///
/// Also evaluates SafeIn/SafeOut classification accuracy.
///
/// Usage:
///   bench_rabitq_accuracy [--dataset /path/to/coco_1k] [--queries N] [--nlist K]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/io/npy_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"

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

// Simple K-Means for assigning vectors to clusters
static void KMeans(const float* data, uint32_t N, Dim dim, uint32_t K,
                   uint32_t max_iter, uint64_t seed,
                   std::vector<float>& centroids,
                   std::vector<uint32_t>& assignments) {
    centroids.resize(static_cast<size_t>(K) * dim);
    assignments.resize(N);

    // Random init: pick K distinct vectors
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
        // Assign
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
        // Update
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
// Percentile helper
// ============================================================================

static float Percentile(std::vector<float>& v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    float idx = p * static_cast<float>(v.size() - 1);
    auto lo = static_cast<size_t>(std::floor(idx));
    auto hi = static_cast<size_t>(std::ceil(idx));
    lo = std::min(lo, v.size() - 1);
    hi = std::min(hi, v.size() - 1);
    if (lo == hi) return v[lo];
    float frac = idx - static_cast<float>(lo);
    return v[lo] * (1.0f - frac) + v[hi] * frac;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetArg(argc, argv, "--dataset",
                                  "/home/zcq/VDB/data/coco_1k");
    uint32_t nlist = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 32));
    int q_limit = GetIntArg(argc, argv, "--queries", 100);
    uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    uint32_t p_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-dk", 90));
    uint32_t p_for_epsilon = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-epsilon", 90));
    uint32_t samples_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--samples-for-dk", 100));
    uint32_t samples_for_epsilon = static_cast<uint32_t>(GetIntArg(argc, argv, "--samples-for-epsilon", 100));

    Log("=== RaBitQ Accuracy Benchmark ===\n");
    Log("Dataset: %s\n", data_dir.c_str());

    // ================================================================
    // Phase 1: Load data
    // ================================================================
    Log("[Phase 1] Loading data...\n");

    auto img_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_or.ok()) {
        std::fprintf(stderr, "Failed: %s\n", img_or.status().ToString().c_str());
        return 1;
    }
    auto& img = img_or.value();
    uint32_t N = img.rows;
    Dim dim = static_cast<Dim>(img.cols);

    auto qry_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed: %s\n", qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q = std::min(static_cast<uint32_t>(q_limit), qry.rows);

    Log("  N=%u, Q=%u, dim=%u\n\n", N, Q, dim);

    // ================================================================
    // Phase 2: K-Means clustering + RaBitQ encoding
    // ================================================================
    Log("[Phase 2] K-Means (K=%u) + RaBitQ encoding...\n", nlist);

    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    KMeans(img.data.data(), N, dim, nlist, 20, 42, centroids, assignments);

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);
    RaBitQEstimator estimator(dim);

    // Encode each vector with its cluster centroid
    std::vector<RaBitQCode> codes(N);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t k = assignments[i];
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        codes[i] = encoder.Encode(
            img.data.data() + static_cast<size_t>(i) * dim, centroid);
    }
    Log("  Encoded %u vectors.\n\n", N);

    // ================================================================
    // Phase 2b: Compute per-cluster epsilon (reconstruction error P90)
    // ================================================================
    Log("[Phase 2b] Computing per-cluster epsilon (reconstruction error)...\n");

    // Count members per cluster
    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    std::vector<float> per_cluster_epsilon(nlist, 0.0f);

    for (uint32_t k = 0; k < nlist; ++k) {
        const auto& members = cluster_members[k];
        if (members.empty()) continue;

        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        std::vector<float> sign_vec(dim);
        std::vector<float> recon_norm(dim);
        std::vector<float> errors;
        errors.reserve(members.size());

        for (uint32_t idx : members) {
            const auto& code = codes[idx];
            const float* original = img.data.data() + static_cast<size_t>(idx) * dim;

            // Reconstruct: bits → ±1/√dim → inverse rotate → scale + centroid
            for (size_t d = 0; d < dim; ++d) {
                int bit = (code.code[d / 64] >> (d % 64)) & 1;
                sign_vec[d] = (2.0f * bit - 1.0f) * inv_sqrt_dim;
            }
            rotation.ApplyInverse(sign_vec.data(), recon_norm.data());

            float err_sq = 0.0f;
            for (size_t d = 0; d < dim; ++d) {
                float diff = original[d] - (centroid[d] + code.norm * recon_norm[d]);
                err_sq += diff * diff;
            }
            errors.push_back(std::sqrt(err_sq));
        }

        std::sort(errors.begin(), errors.end());
        float epsilon = Percentile(errors, static_cast<float>(p_for_epsilon) / 100.0f);
        per_cluster_epsilon[k] = epsilon;
    }

    // Print per-cluster statistics table
    Log("\n  Per-Cluster Statistics:\n");
    Log("  %-10s %-14s %-12s\n", "Cluster", "Num Vectors", "Epsilon");
    Log("  %-10s %-14s %-12s\n", "-------", "-----------", "-------");
    for (uint32_t k = 0; k < nlist; ++k) {
        Log("  %-10u %-14zu %.6f\n", k, cluster_members[k].size(),
            per_cluster_epsilon[k]);
    }
    Log("\n");

    // ================================================================
    // Phase 3: Calibrate d_k
    // ================================================================
    Log("[Phase 3] Calibrating d_k...\n");
    float p_dk = static_cast<float>(p_for_dk) / 100.0f;
    float d_k = ConANN::CalibrateDistanceThreshold(
        img.data.data(), N, dim, samples_for_dk, top_k, p_dk, 42);
    Log("  d_k=%.4f\n\n", d_k);

    // ================================================================
    // Phase 4: Per-query distance comparison
    // ================================================================
    Log("[Phase 4] Computing exact vs RaBitQ distances (%u queries × %u vectors)...\n",
        Q, N);

    // Accumulators
    std::vector<float> all_abs_errors;
    std::vector<float> all_rel_errors;
    float max_abs_error = 0.0f;

    // Recall accumulators
    uint64_t recall_hit_1 = 0, recall_hit_5 = 0, recall_hit_10 = 0;

    // SafeIn/SafeOut classification accuracy
    uint64_t total_safe_in = 0, total_safe_out = 0, total_uncertain = 0;
    uint64_t false_safe_in = 0;   // Classified SafeIn but not in true top-K
    uint64_t false_safe_out = 0;  // Classified SafeOut but IS in true top-K

    auto t0 = std::chrono::steady_clock::now();

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;

        // Exact brute-force distances
        std::vector<std::pair<float, uint32_t>> exact(N);
        for (uint32_t i = 0; i < N; ++i) {
            exact[i] = {simd::L2Sqr(q_vec, img.data.data() + static_cast<size_t>(i) * dim, dim), i};
        }

        // RaBitQ estimated distances (using the vector's assigned cluster centroid)
        std::vector<std::pair<float, uint32_t>> rabitq_dists(N);
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t k = assignments[i];
            const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
            auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            rabitq_dists[i] = {estimator.EstimateDistance(pq, codes[i]), i};
        }

        // Distance error statistics
        for (uint32_t i = 0; i < N; ++i) {
            float abs_err = std::abs(rabitq_dists[i].first - exact[i].first);
            all_abs_errors.push_back(abs_err);
            max_abs_error = std::max(max_abs_error, abs_err);

            if (exact[i].first > 1e-6f) {
                all_rel_errors.push_back(abs_err / exact[i].first);
            }
        }

        // Exact top-K (for recall and classification eval)
        std::partial_sort(exact.begin(), exact.begin() + top_k, exact.end());
        std::unordered_set<uint32_t> true_topk_set;
        for (uint32_t i = 0; i < top_k; ++i) true_topk_set.insert(exact[i].second);

        // RaBitQ top-K (for recall)
        std::partial_sort(rabitq_dists.begin(),
                          rabitq_dists.begin() + top_k,
                          rabitq_dists.end());

        // Recall@{1,5,10}
        auto count_hits = [&](uint32_t k_val) -> uint32_t {
            uint32_t hits = 0;
            for (uint32_t i = 0; i < std::min(k_val, top_k); ++i) {
                if (true_topk_set.count(rabitq_dists[i].second)) hits++;
            }
            return hits;
        };
        recall_hit_1 += (count_hits(1) > 0) ? 1 : 0;
        recall_hit_5 += count_hits(5);
        recall_hit_10 += count_hits(10);

        // SafeIn/SafeOut classification accuracy (per-cluster epsilon)
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t vec_id = rabitq_dists[i].second;
            float eps = per_cluster_epsilon[assignments[vec_id]];
            ConANN cluster_conann(eps, d_k);
            ResultClass rc = cluster_conann.Classify(rabitq_dists[i].first);
            bool is_true_topk = true_topk_set.count(vec_id) > 0;

            switch (rc) {
                case ResultClass::SafeIn:
                    total_safe_in++;
                    if (!is_true_topk) false_safe_in++;
                    break;
                case ResultClass::SafeOut:
                    total_safe_out++;
                    if (is_true_topk) false_safe_out++;
                    break;
                case ResultClass::Uncertain:
                    total_uncertain++;
                    break;
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ================================================================
    // Phase 5: Report
    // ================================================================
    Log("\n[Phase 5] Results (%.1f ms)\n", elapsed_ms);

    float mean_abs = 0.0f;
    for (float e : all_abs_errors) mean_abs += e;
    mean_abs /= static_cast<float>(all_abs_errors.size());

    float mean_rel = 0.0f;
    for (float e : all_rel_errors) mean_rel += e;
    mean_rel /= static_cast<float>(all_rel_errors.size());

    Log("\n=== Distance Error ===\n");
    Log("  Mean absolute error:     %.6f\n", mean_abs);
    Log("  Max absolute error:      %.6f\n", max_abs_error);
    Log("  Mean relative error:     %.6f\n", mean_rel);
    Log("  P95 relative error:      %.6f\n", Percentile(all_rel_errors, 0.95f));
    Log("  P99 relative error:      %.6f\n", Percentile(all_rel_errors, 0.99f));

    float recall_1 = static_cast<float>(recall_hit_1) / static_cast<float>(Q);
    float recall_5 = static_cast<float>(recall_hit_5) /
                     static_cast<float>(Q * std::min(5u, top_k));
    float recall_10 = static_cast<float>(recall_hit_10) /
                      static_cast<float>(Q * std::min(10u, top_k));

    Log("\n=== Ranking Accuracy (RaBitQ brute-force vs exact brute-force) ===\n");
    Log("  recall@1:                %.4f\n", recall_1);
    Log("  recall@5:                %.4f\n", recall_5);
    Log("  recall@10:               %.4f\n", recall_10);

    uint64_t total_classified = total_safe_in + total_safe_out + total_uncertain;
    Log("\n=== SafeIn/SafeOut Classification (per-cluster epsilon, d_k=%.4f) ===\n",
        d_k);
    Log("  Total classified:        %lu\n", total_classified);
    Log("  SafeIn:                  %lu (%.2f%%)\n",
        total_safe_in,
        100.0 * static_cast<double>(total_safe_in) / static_cast<double>(total_classified));
    Log("  SafeOut:                 %lu (%.2f%%)\n",
        total_safe_out,
        100.0 * static_cast<double>(total_safe_out) / static_cast<double>(total_classified));
    Log("  Uncertain:               %lu (%.2f%%)\n",
        total_uncertain,
        100.0 * static_cast<double>(total_uncertain) / static_cast<double>(total_classified));
    Log("\n");
    Log("  False SafeIn:            %lu / %lu (%.4f%%)  [not top-K but classified SafeIn]\n",
        false_safe_in, total_safe_in,
        total_safe_in > 0
            ? 100.0 * static_cast<double>(false_safe_in) / static_cast<double>(total_safe_in)
            : 0.0);
    Log("  False SafeOut:           %lu / %lu (%.4f%%)  [true top-K but classified SafeOut !!!]\n",
        false_safe_out, total_safe_out,
        total_safe_out > 0
            ? 100.0 * static_cast<double>(false_safe_out) / static_cast<double>(total_safe_out)
            : 0.0);
    Log("  False SafeOut (abs):     %lu / %lu queries × top_%u = %lu total top-K checks\n",
        false_safe_out, Q, top_k, static_cast<uint64_t>(Q) * top_k);

    Log("\nDone.\n");
    return 0;
}
