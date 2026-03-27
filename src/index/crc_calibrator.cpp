#include "vdb/index/crc_calibrator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
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

namespace vdb {
namespace index {
namespace {

// ============================================================================
// Internal data structures
// ============================================================================

// Per-query scores and predictions at each nprobe step.
struct QueryScores {
    std::vector<float> raw_scores;                  // [nlist] d_p at each step
    std::vector<std::vector<uint32_t>> predictions; // [nlist][<=k] IDs at each step
};

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

    // 2. Incremental probe with max-heap using RaBitQ estimates.
    std::vector<std::pair<float, uint32_t>> heap;

    for (uint32_t p = 0; p < nlist; ++p) {
        uint32_t cid = centroid_dists[p].second;
        const auto& cluster = clusters[cid];

        // PrepareQuery once per cluster
        auto pq = estimator.PrepareQuery(
            query, centroids + static_cast<size_t>(cid) * dim, rotation);

        for (uint32_t vi = 0; vi < cluster.count; ++vi) {
            // Read code_words + norm_oc from codes_block
            const auto* entry = cluster.codes_block +
                                static_cast<size_t>(vi) * cluster.code_entry_size;
            const auto* words = reinterpret_cast<const uint64_t*>(entry);
            float norm_oc;
            std::memcpy(&norm_oc, entry + norm_byte_offset, sizeof(float));

            float dist = estimator.EstimateDistanceRaw(pq, words, num_words, norm_oc);

            if (heap.size() < top_k) {
                heap.push_back({dist, cluster.ids[vi]});
                std::push_heap(heap.begin(), heap.end());
            } else if (dist < heap.front().first) {
                std::pop_heap(heap.begin(), heap.end());
                heap.back() = {dist, cluster.ids[vi]};
                std::push_heap(heap.begin(), heap.end());
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
    for (size_t i = 0; i < query_indices.size(); ++i) {
        uint32_t qi = query_indices[i];
        const float* q = queries + static_cast<size_t>(qi) * dim;
        all_scores[i] = ComputeScoresForQuery(q, dim, centroids, nlist, clusters, top_k);
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
    for (size_t i = 0; i < query_indices.size(); ++i) {
        uint32_t qi = query_indices[i];
        const float* q = queries + static_cast<size_t>(qi) * dim;
        all_scores[i] = ComputeScoresForQueryRaBitQ(
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

static std::vector<std::vector<float>> NormalizeScores(
    const std::vector<QueryScores>& scores, const NormParams& norm) {

    float range = norm.d_max - norm.d_min;
    float inv_range = (range > 0.0f) ? (1.0f / range) : 0.0f;

    std::vector<std::vector<float>> nonconf(scores.size());
    for (size_t q = 0; q < scores.size(); ++q) {
        uint32_t nlist = static_cast<uint32_t>(scores[q].raw_scores.size());
        nonconf[q].resize(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            float d = scores[q].raw_scores[p];
            if (!std::isfinite(d)) {
                nonconf[q][p] = 1.0f;  // heap not full
            } else {
                nonconf[q][p] = std::clamp((d - norm.d_min) * inv_range, 0.0f, 1.0f);
            }
        }
    }
    return nonconf;
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
    const std::vector<std::vector<float>>& nonconf,
    uint32_t nlist, float reg_lambda, uint32_t kreg) {

    float max_reg_val = ComputeMaxRegVal(reg_lambda, nlist, kreg);

    std::vector<std::vector<float>> reg(nonconf.size());
    for (size_t q = 0; q < nonconf.size(); ++q) {
        // Sort clusters by nonconf score descending to compute rank.
        std::vector<std::pair<float, uint32_t>> sorted(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            sorted[p] = {nonconf[q][p], p};
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        reg[q].resize(nlist);
        for (uint32_t j = 0; j < nlist; ++j) {
            uint32_t original_idx = sorted[j].second;
            uint32_t rank = j + 1;  // 1-based
            float E = (1.0f - nonconf[q][original_idx]) +
                      reg_lambda * std::max(0, static_cast<int>(rank) -
                                               static_cast<int>(kreg));
            reg[q][original_idx] = E / max_reg_val;
        }
    }
    return reg;
}

// ============================================================================
// compute_predictions: given λ, find stopping point per query
// ============================================================================

struct PredictionResult {
    std::vector<std::vector<uint32_t>> predictions;  // [n_queries] predicted IDs
    std::vector<int> clusters_searched;               // [n_queries] # clusters
};

static PredictionResult ComputePredictions(
    float lambda,
    const std::vector<std::vector<float>>& reg_scores,
    const std::vector<QueryScores>& all_scores) {

    size_t nq = reg_scores.size();
    PredictionResult result;
    result.predictions.resize(nq);
    result.clusters_searched.resize(nq, -1);

    for (size_t q = 0; q < nq; ++q) {
        uint32_t nlist = static_cast<uint32_t>(reg_scores[q].size());

        // Sort reg_scores ascending to find last one <= lambda.
        std::vector<std::pair<float, uint32_t>> indexed(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            indexed[p] = {reg_scores[q][p], p};
        }
        std::sort(indexed.begin(), indexed.end());

        int last_valid = -1;
        uint32_t last_index = 0;
        for (size_t i = 0; i < indexed.size(); ++i) {
            if (indexed[i].first <= lambda) {
                last_valid = static_cast<int>(i);
                last_index = indexed[i].second;
            } else {
                break;
            }
        }

        if (last_valid >= 0) {
            result.predictions[q] = all_scores[q].predictions[last_index];
            result.clusters_searched[q] = last_valid + 1;
        } else {
            result.predictions[q] = {};
            result.clusters_searched[q] = 0;
        }
    }
    return result;
}

// ============================================================================
// false_negative_rate
// ============================================================================

static double FalseNegativeRate(
    const std::vector<std::vector<uint32_t>>& predictions,
    const std::vector<std::vector<uint32_t>>& gt,
    const std::vector<uint32_t>& query_indices) {

    int total_overlap = 0;
    int total_gt = 0;
    for (size_t i = 0; i < predictions.size(); ++i) {
        uint32_t qi = query_indices[i];
        const auto& gt_ids = gt[qi];
        const std::set<uint32_t> pred_set(predictions[i].begin(), predictions[i].end());
        int overlap = 0;
        for (uint32_t id : gt_ids) {
            if (pred_set.count(id)) ++overlap;
        }
        total_overlap += overlap;
        total_gt += static_cast<int>(gt_ids.size());
    }
    if (total_gt == 0) return 0.0;
    return 1.0 - static_cast<double>(total_overlap) / total_gt;
}

// ============================================================================
// Brent root-finding via GSL
// ============================================================================

struct BrentParams {
    float target_fnr;
    const std::vector<std::vector<float>>* reg_scores;
    const std::vector<QueryScores>* all_scores;
    const std::vector<std::vector<uint32_t>>* gt;
    const std::vector<uint32_t>* query_indices;
};

static double BrentObjective(double lambda, void* params) {
    auto* p = static_cast<BrentParams*>(params);
    auto pred = ComputePredictions(static_cast<float>(lambda),
                                   *p->reg_scores, *p->all_scores);
    double fnr = FalseNegativeRate(pred.predictions, *p->gt, *p->query_indices);
    return fnr - p->target_fnr;
}

static float BrentSolve(float target_fnr,
                         const std::vector<std::vector<float>>& reg_scores,
                         const std::vector<QueryScores>& all_scores,
                         const std::vector<std::vector<uint32_t>>& gt,
                         const std::vector<uint32_t>& query_indices) {

    BrentParams params{target_fnr, &reg_scores, &all_scores, &gt, &query_indices};

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
    return static_cast<float>(lamhat);
}

// ============================================================================
// pick_lambda_reg: choose best (kreg, reg_lambda) on tune set
// ============================================================================

static float PickLambdaReg(
    float alpha, uint32_t kreg, uint32_t nlist,
    const std::vector<std::vector<float>>& calib_nonconf,
    const std::vector<QueryScores>& calib_scores,
    const std::vector<uint32_t>& calib_indices,
    const std::vector<std::vector<float>>& tune_nonconf,
    const std::vector<QueryScores>& tune_scores,
    const std::vector<uint32_t>& tune_indices,
    const std::vector<std::vector<uint32_t>>& gt) {

    static const float kLambdaValues[] = {0.0f, 0.001f, 0.01f, 0.1f};
    float best_lambda = 0.0f;
    float best_avg_cls = static_cast<float>(nlist);

    int n_calib = static_cast<int>(calib_nonconf.size());
    float target_fnr = (static_cast<float>(n_calib) + 1.0f) / n_calib * alpha -
                       1.0f / (n_calib + 1.0f);
    if (target_fnr < 0.0f) target_fnr = 0.0f;

    for (float reg_lam : kLambdaValues) {
        // Regularize calib scores.
        auto calib_reg = RegularizeScores(calib_nonconf, nlist, reg_lam, kreg);

        // Find lamhat on calib set.
        float lamhat = BrentSolve(target_fnr, calib_reg, calib_scores, gt, calib_indices);

        // Regularize tune scores with same parameters.
        auto tune_reg = RegularizeScores(tune_nonconf, nlist, reg_lam, kreg);

        // Evaluate on tune set.
        auto pred = ComputePredictions(lamhat, tune_reg, tune_scores);
        double fnr = FalseNegativeRate(pred.predictions, gt, tune_indices);

        float avg_cls = 0.0f;
        int valid = 0;
        for (int c : pred.clusters_searched) {
            if (c > 0) { avg_cls += c; ++valid; }
        }
        if (valid > 0) avg_cls /= valid;

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
// CrcCalibrator::Calibrate
// ============================================================================

std::pair<CalibrationResults, CrcCalibrator::EvalResults>
CrcCalibrator::Calibrate(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    const std::vector<std::vector<uint32_t>>& ground_truth) {

    assert(num_queries > 0);
    assert(nlist > 0);
    assert(ground_truth.size() == num_queries);

    // Step 0: Split queries into calib / tune / test.
    std::vector<uint32_t> indices(num_queries);
    std::iota(indices.begin(), indices.end(), 0u);
    std::mt19937_64 rng(config.seed);
    std::shuffle(indices.begin(), indices.end(), rng);

    uint32_t n_calib = static_cast<uint32_t>(num_queries * config.calib_ratio);
    uint32_t n_tune = static_cast<uint32_t>(num_queries * config.tune_ratio);
    if (n_calib < 1) n_calib = 1;
    if (n_tune < 1) n_tune = 1;
    uint32_t n_test = num_queries - n_calib - n_tune;
    if (n_test < 1) {
        // If not enough queries, reduce tune.
        n_tune = std::max(1u, num_queries - n_calib - 1);
        n_test = num_queries - n_calib - n_tune;
    }

    std::vector<uint32_t> calib_idx(indices.begin(), indices.begin() + n_calib);
    std::vector<uint32_t> tune_idx(indices.begin() + n_calib,
                                   indices.begin() + n_calib + n_tune);
    std::vector<uint32_t> test_idx(indices.begin() + n_calib + n_tune,
                                   indices.end());

    // Step 1: Compute scores for calib and tune sets.
    auto calib_scores = ComputeAllScores(queries, calib_idx, dim, centroids,
                                         nlist, clusters, config.top_k);
    auto tune_scores = ComputeAllScores(queries, tune_idx, dim, centroids,
                                        nlist, clusters, config.top_k);

    // Step 2: Normalize using calib set statistics.
    auto norm = ComputeNormParams(calib_scores);
    auto calib_nonconf = NormalizeScores(calib_scores, norm);
    auto tune_nonconf = NormalizeScores(tune_scores, norm);

    // Step 3-4: Pick (kreg, reg_lambda) on tune set.
    uint32_t kreg = 1;  // hardcoded as in original ConANN
    float reg_lambda = PickLambdaReg(config.alpha, kreg, nlist,
                                     calib_nonconf, calib_scores, calib_idx,
                                     tune_nonconf, tune_scores, tune_idx,
                                     ground_truth);

    // Step 5: Final Brent solve on calib set with chosen parameters.
    auto calib_reg = RegularizeScores(calib_nonconf, nlist, reg_lambda, kreg);
    float target_fnr = (static_cast<float>(n_calib) + 1.0f) / n_calib *
                           config.alpha -
                       1.0f / (n_calib + 1.0f);
    if (target_fnr < 0.0f) target_fnr = 0.0f;

    float lamhat = BrentSolve(target_fnr, calib_reg, calib_scores,
                              ground_truth, calib_idx);

    CalibrationResults cal{lamhat, kreg, reg_lambda, norm.d_min, norm.d_max};

    // Step 6: Evaluate on test set.
    EvalResults eval;
    eval.test_size = static_cast<uint32_t>(test_idx.size());

    if (eval.test_size > 0) {
        auto test_scores = ComputeAllScores(queries, test_idx, dim, centroids,
                                            nlist, clusters, config.top_k);
        auto test_nonconf = NormalizeScores(test_scores, norm);
        auto test_reg = RegularizeScores(test_nonconf, nlist, reg_lambda, kreg);
        auto test_pred = ComputePredictions(lamhat, test_reg, test_scores);

        eval.actual_fnr = static_cast<float>(
            FalseNegativeRate(test_pred.predictions, ground_truth, test_idx));

        float sum_probed = 0.0f;
        int valid = 0;
        for (int c : test_pred.clusters_searched) {
            if (c > 0) { sum_probed += c; ++valid; }
        }
        eval.avg_probed = valid > 0 ? sum_probed / valid : 0.0f;

        // Recall computation.
        float sum_r1 = 0, sum_r5 = 0, sum_r10 = 0;
        for (size_t i = 0; i < test_idx.size(); ++i) {
            uint32_t qi = test_idx[i];
            sum_r1 += ComputeRecall(test_pred.predictions[i], ground_truth[qi], 1);
            sum_r5 += ComputeRecall(test_pred.predictions[i], ground_truth[qi], 5);
            sum_r10 += ComputeRecall(test_pred.predictions[i], ground_truth[qi],
                                     std::min(10u, config.top_k));
        }
        float n = static_cast<float>(test_idx.size());
        eval.recall_at_1 = sum_r1 / n;
        eval.recall_at_5 = sum_r5 / n;
        eval.recall_at_10 = sum_r10 / n;
    }

    return {cal, eval};
}

std::pair<CalibrationResults, CrcCalibrator::EvalResults>
CrcCalibrator::CalibrateWithRaBitQ(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    const std::vector<std::vector<uint32_t>>& ground_truth,
    const rabitq::RotationMatrix& rotation) {

    assert(num_queries > 0);
    assert(nlist > 0);
    assert(ground_truth.size() == num_queries);

    // Step 0: Split queries into calib / tune / test.
    std::vector<uint32_t> indices(num_queries);
    std::iota(indices.begin(), indices.end(), 0u);
    std::mt19937_64 rng(config.seed);
    std::shuffle(indices.begin(), indices.end(), rng);

    uint32_t n_calib = static_cast<uint32_t>(num_queries * config.calib_ratio);
    uint32_t n_tune = static_cast<uint32_t>(num_queries * config.tune_ratio);
    if (n_calib < 1) n_calib = 1;
    if (n_tune < 1) n_tune = 1;
    uint32_t n_test = num_queries - n_calib - n_tune;
    if (n_test < 1) {
        n_tune = std::max(1u, num_queries - n_calib - 1);
        n_test = num_queries - n_calib - n_tune;
    }

    std::vector<uint32_t> calib_idx(indices.begin(), indices.begin() + n_calib);
    std::vector<uint32_t> tune_idx(indices.begin() + n_calib,
                                   indices.begin() + n_calib + n_tune);
    std::vector<uint32_t> test_idx(indices.begin() + n_calib + n_tune,
                                   indices.end());

    // Step 1: Compute scores using RaBitQ estimates.
    auto calib_scores = ComputeAllScoresRaBitQ(queries, calib_idx, dim, centroids,
                                                nlist, clusters, config.top_k, rotation);
    auto tune_scores = ComputeAllScoresRaBitQ(queries, tune_idx, dim, centroids,
                                               nlist, clusters, config.top_k, rotation);

    // Step 2: Normalize using calib set statistics.
    auto norm = ComputeNormParams(calib_scores);
    auto calib_nonconf = NormalizeScores(calib_scores, norm);
    auto tune_nonconf = NormalizeScores(tune_scores, norm);

    // Step 3-4: Pick (kreg, reg_lambda) on tune set.
    uint32_t kreg = 1;
    float reg_lambda = PickLambdaReg(config.alpha, kreg, nlist,
                                     calib_nonconf, calib_scores, calib_idx,
                                     tune_nonconf, tune_scores, tune_idx,
                                     ground_truth);

    // Step 5: Final Brent solve on calib set.
    auto calib_reg = RegularizeScores(calib_nonconf, nlist, reg_lambda, kreg);
    float target_fnr = (static_cast<float>(n_calib) + 1.0f) / n_calib *
                           config.alpha -
                       1.0f / (n_calib + 1.0f);
    if (target_fnr < 0.0f) target_fnr = 0.0f;

    float lamhat = BrentSolve(target_fnr, calib_reg, calib_scores,
                              ground_truth, calib_idx);

    CalibrationResults cal{lamhat, kreg, reg_lambda, norm.d_min, norm.d_max};

    // Step 6: Evaluate on test set.
    EvalResults eval;
    eval.test_size = static_cast<uint32_t>(test_idx.size());

    if (eval.test_size > 0) {
        auto test_scores = ComputeAllScoresRaBitQ(queries, test_idx, dim, centroids,
                                                   nlist, clusters, config.top_k, rotation);
        auto test_nonconf = NormalizeScores(test_scores, norm);
        auto test_reg = RegularizeScores(test_nonconf, nlist, reg_lambda, kreg);
        auto test_pred = ComputePredictions(lamhat, test_reg, test_scores);

        eval.actual_fnr = static_cast<float>(
            FalseNegativeRate(test_pred.predictions, ground_truth, test_idx));

        float sum_probed = 0.0f;
        int valid = 0;
        for (int c : test_pred.clusters_searched) {
            if (c > 0) { sum_probed += c; ++valid; }
        }
        eval.avg_probed = valid > 0 ? sum_probed / valid : 0.0f;

        float sum_r1 = 0, sum_r5 = 0, sum_r10 = 0;
        for (size_t i = 0; i < test_idx.size(); ++i) {
            uint32_t qi = test_idx[i];
            sum_r1 += ComputeRecall(test_pred.predictions[i], ground_truth[qi], 1);
            sum_r5 += ComputeRecall(test_pred.predictions[i], ground_truth[qi], 5);
            sum_r10 += ComputeRecall(test_pred.predictions[i], ground_truth[qi],
                                     std::min(10u, config.top_k));
        }
        float n = static_cast<float>(test_idx.size());
        eval.recall_at_1 = sum_r1 / n;
        eval.recall_at_5 = sum_r5 / n;
        eval.recall_at_10 = sum_r10 / n;
    }

    return {cal, eval};
}

}  // namespace index
}  // namespace vdb
