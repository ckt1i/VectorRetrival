#include "vdb/index/conann.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "vdb/simd/distance_l2.h"

namespace vdb {
namespace index {

// ============================================================================
// Construction
// ============================================================================

ConANN::ConANN(float epsilon, float d_k)
    : epsilon_(epsilon),
      d_k_(d_k),
      tau_in_(d_k - 2.0f * epsilon),
      tau_out_(d_k + 2.0f * epsilon) {}

ConANN ConANN::FromConfig(const RaBitQConfig& cfg, Dim dim, float d_k) {
    // epsilon = c_factor * 2^(-bits/2) / sqrt(dim)
    float eps = cfg.c_factor
              * std::pow(2.0f, -static_cast<float>(cfg.bits) / 2.0f)
              / std::sqrt(static_cast<float>(dim));
    return ConANN(eps, d_k);
}

// ============================================================================
// Classification
// ============================================================================

ResultClass ConANN::Classify(float approx_dist) const {
    if (approx_dist < tau_in_) {
        return ResultClass::SafeIn;
    }
    if (approx_dist > tau_out_) {
        return ResultClass::SafeOut;
    }
    return ResultClass::Uncertain;
}

ResultClass ConANN::Classify(float approx_dist, float margin) const {
    if (approx_dist > d_k_ + 2 * margin) {
        return ResultClass::SafeOut;
    }
    if (approx_dist < d_k_ - 2 * margin) {
        return ResultClass::SafeIn;
    }
    return ResultClass::Uncertain;
}

// ============================================================================
// Calibration
// ============================================================================

float ConANN::CalibrateDistanceThreshold(
    const float* vectors, uint32_t N, Dim dim,
    uint32_t num_samples,
    uint32_t top_k,
    float percentile,
    uint64_t seed) {
    // Delegate to query/database separated overload.
    // top_k+1 compensates for self-distance: when query IS in the database,
    // dists includes a 0 entry (self-hit), so we need one extra to skip it.
    return CalibrateDistanceThreshold(
        vectors, N, vectors, N, dim,
        num_samples, top_k + 1, percentile, seed);
}

float ConANN::CalibrateDistanceThreshold(
    const float* queries, uint32_t Q,
    const float* database, uint32_t N,
    Dim dim,
    uint32_t num_samples,
    uint32_t top_k,
    float percentile,
    uint64_t seed) {

    if (Q == 0 || N == 0 || num_samples == 0) {
        return 0.0f;
    }

    if (top_k > N) top_k = N;
    if (top_k == 0) top_k = 1;
    if (num_samples > Q) num_samples = Q;

    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);

    // Sample query indices
    std::vector<uint32_t> indices(Q);
    for (uint32_t i = 0; i < Q; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng);
    indices.resize(num_samples);

    std::vector<float> sample_dk(num_samples);
    std::vector<float> dists(N);

    for (uint32_t s = 0; s < num_samples; ++s) {
        const float* query = queries + static_cast<size_t>(indices[s]) * dim;

        for (uint32_t i = 0; i < N; ++i) {
            dists[i] = simd::L2Sqr(query,
                                    database + static_cast<size_t>(i) * dim,
                                    dim);
        }

        // top_k-1 because 0-indexed and no self-distance to skip
        uint32_t nth = std::min(top_k - 1, N - 1);
        std::nth_element(dists.begin(), dists.begin() + nth, dists.end());
        sample_dk[s] = dists[nth];
    }

    // Percentile interpolation
    std::sort(sample_dk.begin(), sample_dk.end());
    float findex = percentile * static_cast<float>(num_samples - 1);
    uint32_t lo = static_cast<uint32_t>(std::floor(findex));
    uint32_t hi = static_cast<uint32_t>(std::ceil(findex));
    lo = std::min(lo, num_samples - 1);
    hi = std::min(hi, num_samples - 1);

    if (lo == hi) {
        return sample_dk[lo];
    }

    float frac = findex - static_cast<float>(lo);
    return sample_dk[lo] * (1.0f - frac) + sample_dk[hi] * frac;
}

}  // namespace index
}  // namespace vdb
