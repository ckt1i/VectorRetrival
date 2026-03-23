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

    if (N == 0 || num_samples == 0) {
        return 0.0f;
    }

    // Clamp top_k to N (can't find k-th nearest if k > N)
    if (top_k > N) {
        top_k = N;
    }
    if (top_k == 0) {
        top_k = 1;
    }

    // Clamp num_samples to N
    if (num_samples > N) {
        num_samples = N;
    }

    // Random engine for sampling query indices
    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);

    // Build a shuffled list of sample indices
    std::vector<uint32_t> indices(N);
    for (uint32_t i = 0; i < N; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng);
    indices.resize(num_samples);

    // For each sampled query, compute its top_k-th distance
    std::vector<float> sample_dk(num_samples);
    std::vector<float> dists(N);

    for (uint32_t s = 0; s < num_samples; ++s) {
        const float* query = vectors + static_cast<size_t>(indices[s]) * dim;

        // Compute L2 distance to all vectors
        for (uint32_t i = 0; i < N; ++i) {
            dists[i] = simd::L2Sqr(query, vectors + static_cast<size_t>(i) * dim, dim);
        }

        // Partially sort to find the top_k-th smallest distance.
        // Use top_k (not top_k-1) for nth_element because dists[indices[s]]
        // will be 0 (self-distance), and we want the k-th non-self neighbor.
        // However, since the query IS in the dataset, dists include a 0 entry.
        // We want the (top_k)-th element (0-indexed) after the self-hit.
        uint32_t nth = std::min(top_k, N - 1);  // 0-indexed position
        std::nth_element(dists.begin(), dists.begin() + nth, dists.end());
        sample_dk[s] = dists[nth];
    }

    // Sort sample_dk and take percentile
    std::sort(sample_dk.begin(), sample_dk.end());

    // Compute percentile index (0-indexed)
    float findex = percentile * static_cast<float>(num_samples - 1);
    uint32_t lo = static_cast<uint32_t>(std::floor(findex));
    uint32_t hi = static_cast<uint32_t>(std::ceil(findex));
    lo = std::min(lo, num_samples - 1);
    hi = std::min(hi, num_samples - 1);

    if (lo == hi) {
        return sample_dk[lo];
    }

    // Linear interpolation between lo and hi
    float frac = findex - static_cast<float>(lo);
    return sample_dk[lo] * (1.0f - frac) + sample_dk[hi] * frac;
}

}  // namespace index
}  // namespace vdb
