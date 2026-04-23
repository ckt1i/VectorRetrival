#include "vdb/index/crc_calibrator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <vector>

#include <gsl/gsl_errno.h>
#include <gsl/gsl_roots.h>

#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/fastscan.h"
#include "vdb/storage/pack_codes.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace vdb {
namespace index {
namespace {

// ============================================================================
// compute_scores: incremental search over all clusters
// ============================================================================

// For a single query, probe all nlist clusters in order and record d_p + preds.
static QueryScores ComputeScoresForQuery(
    const float* query, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    uint32_t top_k) {

    QueryScores result;
    result.raw_scores.resize(nlist);
    result.predictions.resize(nlist);

    // 1. Sort clusters by distance to query.
    std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        float d = simd::L2Sqr(query, centroids + static_cast<size_t>(c) * dim, dim);
        centroid_dists[c] = {d, c};
    }
    std::sort(centroid_dists.begin(), centroid_dists.end());

    // 2. Incremental probe with max-heap for top-k.
    // Heap: (distance, id) pairs, max-heap by distance.
    std::vector<std::pair<float, uint32_t>> heap;

    for (uint32_t p = 0; p < nlist; ++p) {
        uint32_t cid = centroid_dists[p].second;
        const auto& cluster = clusters[cid];

        for (uint32_t vi = 0; vi < cluster.count; ++vi) {
            float dist = simd::L2Sqr(query,
                                     cluster.vectors + static_cast<size_t>(vi) * dim,
                                     dim);
            if (heap.size() < top_k) {
                heap.push_back({dist, cluster.ids[vi]});
                std::push_heap(heap.begin(), heap.end());
            } else if (dist < heap.front().first) {
                std::pop_heap(heap.begin(), heap.end());
                heap.back() = {dist, cluster.ids[vi]};
                std::push_heap(heap.begin(), heap.end());
            }
        }

        // Record d_p (heap top = k-th nearest, or +inf if not full).
        if (heap.size() < top_k) {
            result.raw_scores[p] = std::numeric_limits<float>::infinity();
        } else {
            result.raw_scores[p] = heap.front().first;
        }

        // Record current top-k IDs sorted by distance (ascending).
        auto sorted_heap = heap;
        std::sort(sorted_heap.begin(), sorted_heap.end());
        result.predictions[p].resize(sorted_heap.size());
        for (size_t i = 0; i < sorted_heap.size(); ++i) {
            result.predictions[p][i] = sorted_heap[i].second;
        }
    }

    return result;
}

// For a single query, probe all nlist clusters using RaBitQ estimates.
// Same structure as ComputeScoresForQuery but uses RaBitQ distance for scoring.
static QueryScores ComputeScoresForQueryRaBitQ(
    const float* query, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    uint32_t top_k,
    const rabitq::RotationMatrix& rotation) {

    rabitq::RaBitQEstimator estimator(dim);
    uint32_t num_words = (dim + 63) / 64;
    uint32_t norm_byte_offset = num_words * sizeof(uint64_t);
    const uint32_t packed_sz = storage::FastScanPackedSize(dim);

    QueryScores result;
    result.raw_scores.resize(nlist);
    result.predictions.resize(nlist);

    // 1. Sort clusters by distance to query (still use exact L2 for ordering).
    std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        float d = simd::L2Sqr(query, centroids + static_cast<size_t>(c) * dim, dim);
        centroid_dists[c] = {d, c};
    }
    std::sort(centroid_dists.begin(), centroid_dists.end());

    // 2. Incremental probe with max-heap using FastScan estimates.
    std::vector<std::pair<float, uint32_t>> heap;
    // Pre-allocate scratch buffers outside the inner loop to avoid repeated heap allocs.
    std::vector<uint8_t> packed_block(packed_sz, 0);
    std::vector<rabitq::RaBitQCode> temp_codes(32);

    for (uint32_t p = 0; p < nlist; ++p) {
        uint32_t cid = centroid_dists[p].second;
        const auto& cluster = clusters[cid];

        // PrepareQuery once per cluster
        auto pq = estimator.PrepareQuery(
            query, centroids + static_cast<size_t>(cid) * dim, rotation);

        if (cluster.num_fs_blocks > 0) {
            // FAST PATH: use pre-packed FastScan blocks from Phase B.
            // Skips temp_codes reconstruction and PackSignBitsForFastScan entirely.
            for (uint32_t b = 0; b < cluster.num_fs_blocks; ++b) {
                const auto& fsb = cluster.fs_blocks[b];
                alignas(64) float fs_dists[32];
                estimator.EstimateDistanceFastScan(
                    pq, fsb.packed, fsb.norms, fsb.count, fs_dists);

                for (uint32_t j = 0; j < fsb.count; ++j) {
                    uint32_t vi = b * 32 + j;
                    float dist = fs_dists[j];
                    if (heap.size() < top_k) {
                        heap.push_back({dist, cluster.ids[vi]});
                        std::push_heap(heap.begin(), heap.end());
                    } else if (dist < heap.front().first) {
                        std::pop_heap(heap.begin(), heap.end());
                        heap.back() = {dist, cluster.ids[vi]};
                        std::push_heap(heap.begin(), heap.end());
                    }
                }
            }
        } else {
            // FALLBACK PATH: reconstruct from codes_block + PackSignBitsForFastScan.
            for (uint32_t vi_base = 0; vi_base < cluster.count; vi_base += 32) {
                uint32_t block_count = std::min(32u, cluster.count - vi_base);

                // Build temporary RaBitQCode array + norms from flat codes_block
                float block_norms[32] = {};
                for (uint32_t j = 0; j < block_count; ++j) {
                    const auto* entry = cluster.codes_block +
                        static_cast<size_t>(vi_base + j) * cluster.code_entry_size;
                    const auto* words = reinterpret_cast<const uint64_t*>(entry);
                    temp_codes[j].code.assign(words, words + num_words);
                    std::memcpy(&block_norms[j], entry + norm_byte_offset, sizeof(float));
                }

                // Pack sign bits into FastScan layout
                storage::PackSignBitsForFastScan(
                    temp_codes.data(), block_count, dim, packed_block.data());

                // FastScan batch-32 distance estimation
                alignas(64) float fs_dists[32];
                estimator.EstimateDistanceFastScan(
                    pq, packed_block.data(), block_norms, block_count, fs_dists);

                for (uint32_t j = 0; j < block_count; ++j) {
                    uint32_t vi = vi_base + j;
                    float dist = fs_dists[j];
                    if (heap.size() < top_k) {
                        heap.push_back({dist, cluster.ids[vi]});
                        std::push_heap(heap.begin(), heap.end());
                    } else if (dist < heap.front().first) {
                        std::pop_heap(heap.begin(), heap.end());
                        heap.back() = {dist, cluster.ids[vi]};
                        std::push_heap(heap.begin(), heap.end());
                    }
                }
            }
        }

        // Record d_p (heap top = k-th nearest estimate).
        if (heap.size() < top_k) {
            result.raw_scores[p] = std::numeric_limits<float>::infinity();
        } else {
            result.raw_scores[p] = heap.front().first;
        }

        // Record current top-k IDs sorted by distance (ascending).
        auto sorted_heap = heap;
        std::sort(sorted_heap.begin(), sorted_heap.end());
        result.predictions[p].resize(sorted_heap.size());
        for (size_t i = 0; i < sorted_heap.size(); ++i) {
            result.predictions[p][i] = sorted_heap[i].second;
        }
    }

    return result;
}

// Compute scores for a set of queries (by index).
static std::vector<QueryScores> ComputeAllScores(
    const float* queries, const std::vector<uint32_t>& query_indices,
    Dim dim, const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters, uint32_t top_k) {

    std::vector<QueryScores> all_scores(query_indices.size());
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(query_indices.size()); ++i) {
        uint32_t qi = query_indices[static_cast<size_t>(i)];
        const float* q = queries + static_cast<size_t>(qi) * dim;
        all_scores[static_cast<size_t>(i)] = ComputeScoresForQuery(
            q, dim, centroids, nlist, clusters, top_k);
    }
    return all_scores;
}

// Compute scores using RaBitQ estimates for a set of queries.
static std::vector<QueryScores> ComputeAllScoresRaBitQ(
    const float* queries, const std::vector<uint32_t>& query_indices,
    Dim dim, const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters, uint32_t top_k,
    const rabitq::RotationMatrix& rotation) {

    std::vector<QueryScores> all_scores(query_indices.size());
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(query_indices.size()); ++i) {
        uint32_t qi = query_indices[static_cast<size_t>(i)];
        const float* q = queries + static_cast<size_t>(qi) * dim;
        all_scores[static_cast<size_t>(i)] = ComputeScoresForQueryRaBitQ(
            q, dim, centroids, nlist, clusters, top_k, rotation);
    }
    return all_scores;
}

// ============================================================================
// normalize: compute d_min, d_max from raw_scores, then normalize to [0,1]
// ============================================================================

struct NormParams {
    float d_min;
    float d_max;
};

static NormParams ComputeNormParams(const std::vector<QueryScores>& scores) {
    float d_min = std::numeric_limits<float>::infinity();
    float d_max = -std::numeric_limits<float>::infinity();
    for (const auto& qs : scores) {
        for (float d : qs.raw_scores) {
            if (std::isfinite(d)) {
                d_min = std::min(d_min, d);
                d_max = std::max(d_max, d);
            }
        }
    }
    if (!std::isfinite(d_min)) d_min = 0.0f;
    if (!std::isfinite(d_max) || d_max <= d_min) d_max = d_min + 1.0f;
    return {d_min, d_max};
}

// subset static cache: reusable data independent of reg_lambda
// ============================================================================

struct QueryStaticCache {
    std::vector<float> nonconf;          // [nlist]
    std::vector<uint32_t> rank_by_step;  // [nlist], 1-based
    std::vector<uint32_t> overlap_by_step;  // [nlist]
    uint32_t gt_size = 0;
};

struct SubsetStaticCache {
    std::vector<QueryStaticCache> queries;
    uint32_t total_gt = 0;
};

static uint32_t ComputeOverlapCount(const std::vector<uint32_t>& predicted,
                                    const std::vector<uint32_t>& gt) {
    uint32_t overlap = 0;
    for (uint32_t gt_id : gt) {
        for (uint32_t pred_id : predicted) {
            if (pred_id == gt_id) {
                ++overlap;
                break;
            }
        }
    }
    return overlap;
}

static SubsetStaticCache BuildSubsetStaticCache(
    const std::vector<QueryScores>& scores,
    const NormParams& norm,
    const std::vector<std::vector<uint32_t>>& gt,
    const std::vector<uint32_t>& query_indices) {

    float range = norm.d_max - norm.d_min;
    float inv_range = (range > 0.0f) ? (1.0f / range) : 0.0f;

    SubsetStaticCache cache;
    cache.queries.resize(scores.size());

    for (size_t q = 0; q < scores.size(); ++q) {
        uint32_t nlist = static_cast<uint32_t>(scores[q].raw_scores.size());
        uint32_t qi = query_indices[q];
        const auto& gt_ids = gt[qi];

        auto& qc = cache.queries[q];
        qc.nonconf.resize(nlist);
        qc.rank_by_step.resize(nlist);
        qc.overlap_by_step.resize(nlist);
        qc.gt_size = static_cast<uint32_t>(gt_ids.size());
        cache.total_gt += qc.gt_size;

        std::vector<std::pair<float, uint32_t>> sorted_nonconf(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            float d = scores[q].raw_scores[p];
            float nonconf = !std::isfinite(d)
                                ? 1.0f
                                : std::clamp((d - norm.d_min) * inv_range, 0.0f, 1.0f);
            qc.nonconf[p] = nonconf;
            sorted_nonconf[p] = {nonconf, p};
            qc.overlap_by_step[p] =
                ComputeOverlapCount(scores[q].predictions[p], gt_ids);
        }
        std::sort(sorted_nonconf.begin(), sorted_nonconf.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (uint32_t j = 0; j < nlist; ++j) {
            qc.rank_by_step[sorted_nonconf[j].second] = j + 1;
        }
    }

    return cache;
}

// ============================================================================
// regularize_scores: RAPS regularization
// ============================================================================

static constexpr float kMaxRegValMargin = 1.2f;

static float ComputeMaxRegVal(float reg_lambda, uint32_t nlist, uint32_t kreg) {
    float raw = 1.0f + reg_lambda * std::max(0, static_cast<int>(nlist) -
                                                  static_cast<int>(kreg));
    return raw * kMaxRegValMargin;
}

static std::vector<std::vector<float>> RegularizeScores(
    const SubsetStaticCache& cache,
    uint32_t nlist, float reg_lambda, uint32_t kreg) {

    float max_reg_val = ComputeMaxRegVal(reg_lambda, nlist, kreg);

    std::vector<std::vector<float>> reg(cache.queries.size());
    for (size_t q = 0; q < cache.queries.size(); ++q) {
        const auto& qc = cache.queries[q];
        for (uint32_t p = 0; p < nlist; ++p) {
            uint32_t rank = qc.rank_by_step[p];
            float E = (1.0f - qc.nonconf[p]) +
                      reg_lambda * std::max(0, static_cast<int>(rank) -
                                               static_cast<int>(kreg));
            reg[q].push_back(E / max_reg_val);
        }
    }
    return reg;
}

// ============================================================================
// stop profiles: pre-sort once, reuse across repeated lambda evaluations
// ============================================================================

struct QueryStopProfile {
    std::vector<float> sorted_scores;          // ascending reg_scores
    std::vector<uint32_t> sorted_to_step;      // sorted idx -> original step
    std::vector<uint32_t> clusters_searched;   // sorted idx -> # clusters
    std::vector<uint32_t> overlap_at_sorted;   // sorted idx -> overlap count
};

struct StopProfiles {
    std::vector<QueryStopProfile> profiles;
    uint32_t total_gt = 0;
};

struct LambdaEvalResult {
    uint32_t total_overlap = 0;
    uint32_t total_gt = 0;
    uint64_t sum_clusters = 0;
    uint32_t valid_queries = 0;
    std::vector<int> selected_sorted_indices;  // -1 means no stop step selected
};

struct SolverStats {
    uint32_t candidate_count = 0;
    uint32_t objective_evals = 0;
    double profile_build_ms = 0.0;
    double solver_ms = 0.0;
};

static StopProfiles BuildStopProfiles(
    const std::vector<std::vector<float>>& reg_scores,
    const SubsetStaticCache& cache) {

    size_t nq = reg_scores.size();
    StopProfiles out;
    out.profiles.resize(nq);
    out.total_gt = cache.total_gt;

    for (size_t q = 0; q < nq; ++q) {
        uint32_t nlist = static_cast<uint32_t>(reg_scores[q].size());
        const auto& qc = cache.queries[q];

        auto& profile = out.profiles[q];
        profile.sorted_scores.resize(nlist);
        profile.sorted_to_step.resize(nlist);
        profile.clusters_searched.resize(nlist);
        profile.overlap_at_sorted.resize(nlist);

        // Sort reg_scores ascending; preserve the legacy "last <= lambda" rule.
        std::vector<std::pair<float, uint32_t>> indexed;
        indexed.reserve(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            indexed.push_back({reg_scores[q][p], p});
        }
        std::sort(indexed.begin(), indexed.end());

        for (uint32_t i = 0; i < nlist; ++i) {
            uint32_t step = indexed[i].second;
            profile.sorted_scores[i] = indexed[i].first;
            profile.sorted_to_step[i] = step;
            profile.clusters_searched[i] = i + 1;
            profile.overlap_at_sorted[i] = qc.overlap_by_step[step];
        }
    }
    return out;
}

static int SelectStep(float lambda, const QueryStopProfile& profile) {
    auto it = std::upper_bound(profile.sorted_scores.begin(),
                               profile.sorted_scores.end(), lambda);
    if (it == profile.sorted_scores.begin()) {
        return -1;
    }
    return static_cast<int>(std::distance(profile.sorted_scores.begin(), it) - 1);
}

static LambdaEvalResult EvaluateLambda(float lambda,
                                       const StopProfiles& profiles,
                                       bool collect_selected_steps = false) {
    LambdaEvalResult result;
    result.total_gt = profiles.total_gt;
    if (collect_selected_steps) {
        result.selected_sorted_indices.resize(profiles.profiles.size(), -1);
    }

    for (size_t q = 0; q < profiles.profiles.size(); ++q) {
        const auto& profile = profiles.profiles[q];
        int selected = SelectStep(lambda, profile);
        if (collect_selected_steps) {
            result.selected_sorted_indices[q] = selected;
        }
        if (selected < 0) continue;

        result.total_overlap += profile.overlap_at_sorted[static_cast<size_t>(selected)];
        result.sum_clusters += profile.clusters_searched[static_cast<size_t>(selected)];
        ++result.valid_queries;
    }

    return result;
}

static double FalseNegativeRate(const LambdaEvalResult& eval) {
    if (eval.total_gt == 0) return 0.0;
    return 1.0 - static_cast<double>(eval.total_overlap) / eval.total_gt;
}

// Difficulty-aware query ordering for stratified calibration splits.
// Higher difficulty values indicate lower overlap at the midpoint probe step.
static std::vector<float> ComputeQueryDifficulties(
    const std::vector<QueryScores>& all_scores,
    const std::vector<std::vector<uint32_t>>& ground_truth,
    uint32_t nlist) {
    std::vector<float> difficulty(all_scores.size(), 0.0f);

    if (all_scores.empty()) return difficulty;

    uint32_t stride = std::max(1u, nlist / 2);
    uint32_t step = std::min(nlist - 1, stride);

    for (size_t qi = 0; qi < all_scores.size(); ++qi) {
        const auto& qs = all_scores[qi];
        const auto& gt = ground_truth[qi];
        if (gt.empty() || qs.predictions.empty()) {
            difficulty[qi] = 0.0f;
            continue;
        }
        uint32_t overlap = ComputeOverlapCount(qs.predictions[step], gt);
        float denom = static_cast<float>(gt.size());
        if (denom > 0.0f) {
            difficulty[qi] = 1.0f - (static_cast<float>(overlap) / denom);
        }
    }
    return difficulty;
}

static void SplitQueryIndices(
    const std::vector<float>& difficulty,
    float calib_ratio,
    float tune_ratio,
    uint64_t seed,
    bool use_stratified_split,
    uint32_t split_buckets,
    std::vector<uint32_t>& calib_idx,
    std::vector<uint32_t>& tune_idx,
    std::vector<uint32_t>& test_idx) {
    const uint32_t num_queries = static_cast<uint32_t>(difficulty.size());
    std::vector<uint32_t> indices(num_queries);
    std::iota(indices.begin(), indices.end(), 0u);

    uint32_t n_calib = static_cast<uint32_t>(num_queries * calib_ratio);
    uint32_t n_tune = static_cast<uint32_t>(num_queries * tune_ratio);
    if (n_calib < 1) n_calib = 1;
    if (n_tune < 1) n_tune = 1;
    uint32_t n_test = num_queries - n_calib - n_tune;
    if (n_test < 1) {
        n_tune = std::max(1u, num_queries - n_calib - 1);
        n_test = num_queries - n_calib - n_tune;
    }

    if (!use_stratified_split || num_queries <= split_buckets || split_buckets <= 1) {
        std::mt19937_64 rng(seed);
        std::shuffle(indices.begin(), indices.end(), rng);
        calib_idx.assign(indices.begin(), indices.begin() + n_calib);
        tune_idx.assign(indices.begin() + n_calib,
                        indices.begin() + n_calib + n_tune);
        test_idx.assign(indices.begin() + n_calib + n_tune, indices.end());
        return;
    }

    uint32_t bucket_count = std::min(split_buckets, num_queries);
    std::vector<float> order = difficulty;
    std::vector<uint32_t> sorted_idx(num_queries);
    std::iota(sorted_idx.begin(), sorted_idx.end(), 0u);
    std::sort(
        sorted_idx.begin(), sorted_idx.end(),
        [&](uint32_t a, uint32_t b) { return order[a] < order[b]; });

    std::vector<std::vector<uint32_t>> buckets(bucket_count);
    for (uint32_t i = 0; i < num_queries; ++i) {
        buckets[i % bucket_count].push_back(sorted_idx[i]);
    }

    for (auto& bucket : buckets) {
        std::mt19937_64 rng(seed + bucket.size());
        std::shuffle(bucket.begin(), bucket.end(), rng);
    }

    std::vector<uint32_t> merged_order;
    merged_order.reserve(num_queries);
    for (uint32_t pos = 0; ; ++pos) {
        bool added = false;
        for (uint32_t b = 0; b < bucket_count; ++b) {
            if (pos < buckets[b].size()) {
                merged_order.push_back(buckets[b][pos]);
                added = true;
            }
        }
        if (!added) break;
    }

    uint32_t calib_end = std::min(num_queries, n_calib);
    uint32_t tune_end = std::min(num_queries, calib_end + n_tune);
    calib_idx.assign(merged_order.begin(),
                     merged_order.begin() + calib_end);
    tune_idx.assign(merged_order.begin() + calib_end,
                    merged_order.begin() + tune_end);
    test_idx.assign(merged_order.begin() + tune_end, merged_order.end());
    // Safety fallbacks if bucketed order unexpectedly short.
    if (calib_idx.size() < n_calib && !merged_order.empty()) {
        indices = merged_order;
        calib_idx.assign(indices.begin(), indices.begin() + std::min<uint32_t>(n_calib, num_queries));
        tune_idx.assign(indices.begin() + calib_idx.size(),
                        indices.begin() + std::min<uint32_t>(n_calib + n_tune, num_queries));
        test_idx.assign(indices.begin() + calib_idx.size() + tune_idx.size(),
                        indices.end());
    }
}

// ============================================================================
// Brent root-finding via GSL
// ============================================================================

struct BrentParams {
    float target_fnr;
    const StopProfiles* profiles;
    uint32_t* objective_evals;
};

static double BrentObjective(double lambda, void* params) {
    auto* p = static_cast<BrentParams*>(params);
    if (p->objective_evals != nullptr) {
        ++(*p->objective_evals);
    }
    auto eval = EvaluateLambda(static_cast<float>(lambda), *p->profiles);
    double fnr = FalseNegativeRate(eval);
    return fnr - p->target_fnr;
}

static float BrentSolve(float target_fnr,
                        const StopProfiles& profiles,
                        SolverStats* stats = nullptr) {

    auto t_solver_start = std::chrono::steady_clock::now();
    uint32_t objective_evals = 0;
    BrentParams params{target_fnr, &profiles, &objective_evals};

    // Check boundary conditions.
    // CRC semantics: small λ → aggressive stopping → high FNR (f > 0)
    //                large λ → less stopping → low FNR (f < 0)
    // Brent needs f(lo) and f(hi) with opposite signs.
    double f_lo = BrentObjective(0.0, &params);
    double f_hi = BrentObjective(1.0, &params);

    if (f_lo <= 0.0 && f_hi <= 0.0) return 0.0f;   // FNR always below target
    if (f_lo >= 0.0 && f_hi >= 0.0) return 1.0f;   // FNR always above target

    gsl_set_error_handler_off();
    gsl_root_fsolver* solver = gsl_root_fsolver_alloc(gsl_root_fsolver_brent);

    gsl_function F;
    F.function = BrentObjective;
    F.params = &params;

    gsl_root_fsolver_set(solver, &F, 0.0, 1.0);

    double lamhat = 0.5;
    int max_iter = 100;
    for (int iter = 0; iter < max_iter; ++iter) {
        gsl_root_fsolver_iterate(solver);
        lamhat = gsl_root_fsolver_root(solver);
        double lo = gsl_root_fsolver_x_lower(solver);
        double hi = gsl_root_fsolver_x_upper(solver);
        int status = gsl_root_test_interval(lo, hi, 1e-6, 1e-6);
        if (status == GSL_SUCCESS) break;
    }

    gsl_root_fsolver_free(solver);
    if (stats != nullptr) {
        stats->objective_evals = objective_evals;
        stats->candidate_count = 0;
        stats->solver_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_solver_start).count();
    }
    return static_cast<float>(lamhat);
}

static std::vector<float> CollectCandidateThresholds(const StopProfiles& profiles) {
    std::vector<float> candidates;
    size_t total = 0;
    for (const auto& profile : profiles.profiles) {
        total += profile.sorted_scores.size();
    }
    candidates.reserve(total);
    for (const auto& profile : profiles.profiles) {
        candidates.insert(candidates.end(),
                          profile.sorted_scores.begin(),
                          profile.sorted_scores.end());
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    return candidates;
}

static float SolveDiscreteThreshold(float target_fnr,
                                    const StopProfiles& profiles,
                                    SolverStats* stats = nullptr) {
    auto t_solver_start = std::chrono::steady_clock::now();
    auto candidates = CollectCandidateThresholds(profiles);

    float best_lambda = candidates.empty() ? 1.0f : candidates.back();
    uint32_t objective_evals = 0;
    if (!candidates.empty()) {
        size_t lo = 0;
        size_t hi = candidates.size();
        size_t first_legal = candidates.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            ++objective_evals;
            auto eval = EvaluateLambda(candidates[mid], profiles);
            double fnr = FalseNegativeRate(eval);
            if (fnr <= target_fnr) {
                first_legal = mid;
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }

        if (first_legal == candidates.size()) {
            best_lambda = candidates.back();
        } else {
            auto best_eval = EvaluateLambda(candidates[first_legal], profiles);
            ++objective_evals;
            uint64_t best_num = best_eval.sum_clusters;
            uint32_t best_den = std::max(best_eval.valid_queries, 1u);
            best_lambda = candidates[first_legal];
            for (size_t i = first_legal + 1; i < candidates.size(); ++i) {
                ++objective_evals;
                auto eval = EvaluateLambda(candidates[i], profiles);
                uint64_t lhs = eval.sum_clusters * static_cast<uint64_t>(best_den);
                uint64_t rhs = best_num * static_cast<uint64_t>(std::max(eval.valid_queries, 1u));
                if (lhs != rhs) {
                    break;
                }
                best_lambda = candidates[i];
            }
        }
    }

    if (stats != nullptr) {
        stats->candidate_count = static_cast<uint32_t>(candidates.size());
        stats->objective_evals = objective_evals;
        stats->solver_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_solver_start).count();
    }
    return best_lambda;
}

static float SolveLamhat(CrcCalibrator::Solver solver,
                         float target_fnr,
                         const StopProfiles& profiles,
                         SolverStats* stats = nullptr) {
    switch (solver) {
        case CrcCalibrator::Solver::Brent:
            return BrentSolve(target_fnr, profiles, stats);
        case CrcCalibrator::Solver::DiscreteThreshold:
            return SolveDiscreteThreshold(target_fnr, profiles, stats);
    }
    return BrentSolve(target_fnr, profiles, stats);
}

// ============================================================================
// pick_lambda_reg: choose best (kreg, reg_lambda) on tune set
// ============================================================================

static float PickLambdaReg(
    CrcCalibrator::Solver solver,
    float alpha, uint32_t kreg, uint32_t nlist,
    const SubsetStaticCache& calib_cache,
    const SubsetStaticCache& tune_cache) {

    static const float kLambdaValues[] = {0.0f, 0.001f, 0.01f, 0.1f};
    float best_lambda = 0.0f;
    float best_avg_cls = static_cast<float>(nlist);

    int n_calib = static_cast<int>(calib_cache.queries.size());
    float target_fnr = (static_cast<float>(n_calib) + 1.0f) / n_calib * alpha -
                       1.0f / (n_calib + 1.0f);
    if (target_fnr < 0.0f) target_fnr = 0.0f;

    for (float reg_lam : kLambdaValues) {
        // Regularize calib scores.
        auto calib_reg = RegularizeScores(calib_cache, nlist, reg_lam, kreg);
        auto calib_profiles = BuildStopProfiles(calib_reg, calib_cache);

        // Find lamhat on calib set.
        float lamhat = SolveLamhat(solver, target_fnr, calib_profiles);

        // Regularize tune scores with same parameters.
        auto tune_reg = RegularizeScores(tune_cache, nlist, reg_lam, kreg);
        auto tune_profiles = BuildStopProfiles(tune_reg, tune_cache);

        // Evaluate on tune set.
        auto eval = EvaluateLambda(lamhat, tune_profiles);
        double fnr = FalseNegativeRate(eval);

        float avg_cls = 0.0f;
        if (eval.valid_queries > 0) {
            avg_cls = static_cast<float>(eval.sum_clusters) / eval.valid_queries;
        }

        if (fnr <= alpha && avg_cls < best_avg_cls) {
            best_lambda = reg_lam;
            best_avg_cls = avg_cls;
        }
    }
    return best_lambda;
}

// ============================================================================
// recall computation
// ============================================================================

static float ComputeRecall(const std::vector<uint32_t>& predicted,
                           const std::vector<uint32_t>& gt, uint32_t K) {
    uint32_t pk = std::min(K, static_cast<uint32_t>(predicted.size()));
    uint32_t gk = std::min(K, static_cast<uint32_t>(gt.size()));
    std::set<uint32_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return gk > 0 ? static_cast<float>(hits) / gk : 0.0f;
}

}  // namespace

// ============================================================================
// CrcCalibrator::Calibrate — core (scores-only, distance-space agnostic)
// ============================================================================

const char* CrcCalibrator::SolverName(Solver solver) {
    switch (solver) {
        case Solver::Brent:
            return "brent";
        case Solver::DiscreteThreshold:
            return "discrete_threshold";
    }
    return "brent";
}

std::pair<CalibrationResults, CrcCalibrator::EvalResults>
CrcCalibrator::Calibrate(
const Config& config,
    const std::vector<QueryScores>& all_scores,
    uint32_t nlist) {

    uint32_t num_queries = static_cast<uint32_t>(all_scores.size());
    assert(num_queries > 0);
    assert(nlist > 0);

    // Derive ground truth from full-probe predictions[nlist-1].
    std::vector<std::vector<uint32_t>> ground_truth(num_queries);
    for (uint32_t i = 0; i < num_queries; ++i) {
        assert(all_scores[i].predictions.size() == nlist);
        ground_truth[i] = all_scores[i].predictions[nlist - 1];
    }

    // Step 0: Split queries into calib / tune / test.
    auto difficulties = ComputeQueryDifficulties(all_scores, ground_truth, nlist);
    std::vector<uint32_t> calib_idx;
    std::vector<uint32_t> tune_idx;
    std::vector<uint32_t> test_idx;
    SplitQueryIndices(difficulties, config.calib_ratio, config.tune_ratio,
                      config.seed, config.use_stratified_split,
                      config.split_buckets, calib_idx, tune_idx, test_idx);

    // Extract subset scores by reference (indices map into all_scores).
    auto extract_scores = [&](const std::vector<uint32_t>& idx) {
        std::vector<QueryScores> sub(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) {
            sub[i] = all_scores[idx[i]];
        }
        return sub;
    };

    auto calib_scores = extract_scores(calib_idx);
    auto tune_scores = extract_scores(tune_idx);

    // Step 2: Normalize using calib set statistics.
    auto norm = ComputeNormParams(calib_scores);
    auto calib_cache = BuildSubsetStaticCache(calib_scores, norm, ground_truth, calib_idx);
    auto tune_cache = BuildSubsetStaticCache(tune_scores, norm, ground_truth, tune_idx);

    // Step 3-4: Pick (kreg, reg_lambda) on tune set.
    uint32_t kreg = 1;
    float reg_lambda = PickLambdaReg(config.solver, config.alpha, kreg, nlist,
                                     calib_cache, tune_cache);

    // Step 5: Final Brent solve on calib set with chosen parameters.
    SolverStats solver_stats;
    uint32_t n_calib = static_cast<uint32_t>(calib_idx.size());
    if (n_calib < 1) n_calib = 1;
    auto calib_reg = RegularizeScores(calib_cache, nlist, reg_lambda, kreg);
    auto t_profile_start = std::chrono::steady_clock::now();
    auto calib_profiles = BuildStopProfiles(calib_reg, calib_cache);
    solver_stats.profile_build_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_profile_start).count();
    float target_fnr = (static_cast<float>(n_calib) + 1.0f) / n_calib *
                           config.alpha -
                       1.0f / (n_calib + 1.0f);
    if (target_fnr < 0.0f) target_fnr = 0.0f;

    float lamhat = SolveLamhat(config.solver, target_fnr, calib_profiles,
                               &solver_stats);

    CalibrationResults cal{lamhat, kreg, reg_lambda, norm.d_min, norm.d_max};

    // Step 6: Evaluate on test set.
    EvalResults eval;
    eval.test_size = static_cast<uint32_t>(test_idx.size());
    eval.solver_used = config.solver;
    eval.candidate_count = solver_stats.candidate_count;
    eval.objective_evals = solver_stats.objective_evals;
    eval.profile_build_ms = solver_stats.profile_build_ms;
    eval.solver_ms = solver_stats.solver_ms;

    if (eval.test_size > 0) {
        auto test_scores = extract_scores(test_idx);
        auto test_cache = BuildSubsetStaticCache(test_scores, norm, ground_truth, test_idx);
        auto test_reg = RegularizeScores(test_cache, nlist, reg_lambda, kreg);
        auto test_profiles = BuildStopProfiles(test_reg, test_cache);
        auto test_eval = EvaluateLambda(lamhat, test_profiles, true);

        eval.actual_fnr = static_cast<float>(FalseNegativeRate(test_eval));

        if (test_eval.valid_queries > 0) {
            eval.avg_probed = static_cast<float>(test_eval.sum_clusters) /
                              test_eval.valid_queries;
        } else {
            eval.avg_probed = 0.0f;
        }

        float sum_r1 = 0, sum_r5 = 0, sum_r10 = 0;
        for (size_t i = 0; i < test_idx.size(); ++i) {
            uint32_t qi = test_idx[i];
            int selected = test_eval.selected_sorted_indices[i];
            std::vector<uint32_t> predicted;
            if (selected >= 0) {
                uint32_t step = test_profiles.profiles[i]
                                    .sorted_to_step[static_cast<size_t>(selected)];
                predicted = test_scores[i].predictions[step];
            }
            sum_r1 += ComputeRecall(predicted, ground_truth[qi], 1);
            sum_r5 += ComputeRecall(predicted, ground_truth[qi], 5);
            sum_r10 += ComputeRecall(predicted, ground_truth[qi],
                                     std::min(10u, config.top_k));
        }
        float n = static_cast<float>(test_idx.size());
        eval.recall_at_1 = sum_r1 / n;
        eval.recall_at_5 = sum_r5 / n;
        eval.recall_at_10 = sum_r10 / n;
    }

    return {cal, eval};
}

// ============================================================================
// CrcCalibrator::Calibrate — convenience wrapper (exact L2)
// ============================================================================

std::pair<CalibrationResults, CrcCalibrator::EvalResults>
CrcCalibrator::Calibrate(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters) {

    assert(num_queries > 0);
    assert(nlist > 0);

    // Compute scores for all queries, then delegate to core.
    std::vector<uint32_t> all_indices(num_queries);
    std::iota(all_indices.begin(), all_indices.end(), 0u);
    auto all_scores = ComputeAllScores(queries, all_indices, dim, centroids,
                                       nlist, clusters, config.top_k);
    return Calibrate(config, all_scores, nlist);
}

// ============================================================================
// CrcCalibrator::CalibrateWithRaBitQ — convenience wrapper (RaBitQ estimates)
// ============================================================================

std::pair<CalibrationResults, CrcCalibrator::EvalResults>
CrcCalibrator::CalibrateWithRaBitQ(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    const rabitq::RotationMatrix& rotation) {

    assert(num_queries > 0);
    assert(nlist > 0);

    // Compute scores for all queries using RaBitQ, then delegate to core.
    std::vector<uint32_t> all_indices(num_queries);
    std::iota(all_indices.begin(), all_indices.end(), 0u);
    auto all_scores = ComputeAllScoresRaBitQ(queries, all_indices, dim, centroids,
                                              nlist, clusters, config.top_k, rotation);
    return Calibrate(config, all_scores, nlist);
}

// ============================================================================
// CrcCalibrator::ComputeScoresRaBitQ — exposed for offline precomputation
// ============================================================================

std::vector<QueryScores> CrcCalibrator::ComputeScoresRaBitQ(
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    uint32_t top_k,
    const rabitq::RotationMatrix& rotation) {

    std::vector<uint32_t> all_indices(num_queries);
    std::iota(all_indices.begin(), all_indices.end(), 0u);
    return ComputeAllScoresRaBitQ(queries, all_indices, dim, centroids,
                                   nlist, clusters, top_k, rotation);
}

// ============================================================================
// CrcCalibrator::WriteScores / ReadScores — binary serialization
// ============================================================================

static constexpr uint32_t kCrcScoresMagic = 0x43524353;  // "CRCS"
static constexpr uint32_t kCrcScoresVersion = 1;

Status CrcCalibrator::WriteScores(const std::string& path,
                                  const std::vector<QueryScores>& scores,
                                  uint32_t nlist, uint32_t top_k) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        return Status::IOError("Failed to create CRC scores file: " + path);
    }

    // Header
    uint32_t num_queries = static_cast<uint32_t>(scores.size());
    uint32_t magic = kCrcScoresMagic;
    uint32_t version = kCrcScoresVersion;
    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
    ofs.write(reinterpret_cast<const char*>(&num_queries), sizeof(num_queries));
    ofs.write(reinterpret_cast<const char*>(&nlist), sizeof(nlist));
    ofs.write(reinterpret_cast<const char*>(&top_k), sizeof(top_k));

    // Per-query data
    std::vector<uint32_t> pred_buf(static_cast<size_t>(nlist) * top_k,
                                   std::numeric_limits<uint32_t>::max());
    for (const auto& qs : scores) {
        // raw_scores[nlist]
        ofs.write(reinterpret_cast<const char*>(qs.raw_scores.data()),
                  static_cast<std::streamsize>(nlist * sizeof(float)));

        // predictions[nlist × top_k] — pad with UINT32_MAX sentinel
        std::fill(pred_buf.begin(), pred_buf.end(),
                  std::numeric_limits<uint32_t>::max());
        for (uint32_t p = 0; p < nlist; ++p) {
            uint32_t n = std::min(top_k,
                                  static_cast<uint32_t>(qs.predictions[p].size()));
            for (uint32_t j = 0; j < n; ++j) {
                pred_buf[static_cast<size_t>(p) * top_k + j] =
                    qs.predictions[p][j];
            }
        }
        ofs.write(reinterpret_cast<const char*>(pred_buf.data()),
                  static_cast<std::streamsize>(pred_buf.size() * sizeof(uint32_t)));
    }

    if (!ofs.good()) {
        return Status::IOError("Failed to write CRC scores file: " + path);
    }
    return Status::OK();
}

Status CrcCalibrator::ReadScores(const std::string& path,
                                 std::vector<QueryScores>& scores,
                                 uint32_t& nlist, uint32_t& top_k) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return Status::IOError("Failed to open CRC scores file: " + path);
    }

    // Header
    uint32_t magic, version, num_queries;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    ifs.read(reinterpret_cast<char*>(&num_queries), sizeof(num_queries));
    ifs.read(reinterpret_cast<char*>(&nlist), sizeof(nlist));
    ifs.read(reinterpret_cast<char*>(&top_k), sizeof(top_k));

    if (magic != kCrcScoresMagic) {
        return Status::Corruption("Invalid CRC scores magic number");
    }
    if (version != kCrcScoresVersion) {
        return Status::Corruption("Unsupported CRC scores version");
    }

    // Per-query data
    scores.resize(num_queries);
    std::vector<uint32_t> pred_buf(static_cast<size_t>(nlist) * top_k);

    for (uint32_t qi = 0; qi < num_queries; ++qi) {
        scores[qi].raw_scores.resize(nlist);
        ifs.read(reinterpret_cast<char*>(scores[qi].raw_scores.data()),
                 static_cast<std::streamsize>(nlist * sizeof(float)));

        ifs.read(reinterpret_cast<char*>(pred_buf.data()),
                 static_cast<std::streamsize>(pred_buf.size() * sizeof(uint32_t)));

        scores[qi].predictions.resize(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            scores[qi].predictions[p].clear();
            for (uint32_t j = 0; j < top_k; ++j) {
                uint32_t id = pred_buf[static_cast<size_t>(p) * top_k + j];
                if (id == std::numeric_limits<uint32_t>::max()) break;
                scores[qi].predictions[p].push_back(id);
            }
        }
    }

    if (!ifs.good()) {
        return Status::IOError("Failed to read CRC scores file: " + path);
    }
    return Status::OK();
}

}  // namespace index
}  // namespace vdb
