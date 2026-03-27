#include "vdb/index/crc_calibrator.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "vdb/simd/distance_l2.h"

namespace vdb {
namespace index {
namespace {

// Generate synthetic clustered data: nlist clusters of cluster_size vectors in dim-D.
// Clusters are well-separated (centered at offset * cluster_id along each axis).
struct SyntheticData {
    std::vector<float> vectors;       // [N x dim]
    std::vector<float> centroids;     // [nlist x dim]
    std::vector<ClusterData> clusters;
    std::vector<std::vector<uint32_t>> cluster_ids;
    uint32_t N;
    uint32_t nlist;
    Dim dim;

    // Stable storage for ClusterData pointers.
    std::vector<std::vector<uint32_t>> id_storage;
};

static SyntheticData GenerateData(uint32_t nlist, uint32_t cluster_size,
                                  Dim dim, uint64_t seed = 42) {
    SyntheticData data;
    data.nlist = nlist;
    data.dim = dim;
    data.N = nlist * cluster_size;
    data.vectors.resize(static_cast<size_t>(data.N) * dim);
    data.centroids.resize(static_cast<size_t>(nlist) * dim, 0.0f);
    data.cluster_ids.resize(nlist);
    data.id_storage.resize(nlist);

    std::mt19937_64 rng(seed);
    std::normal_distribution<float> noise(0.0f, 0.5f);

    float offset = 10.0f;  // large separation between clusters
    uint32_t global_id = 0;

    for (uint32_t c = 0; c < nlist; ++c) {
        // Centroid: offset * c along first axis.
        data.centroids[static_cast<size_t>(c) * dim] = offset * c;

        data.id_storage[c].resize(cluster_size);
        for (uint32_t v = 0; v < cluster_size; ++v) {
            size_t base = static_cast<size_t>(global_id) * dim;
            for (Dim d = 0; d < dim; ++d) {
                data.vectors[base + d] =
                    data.centroids[static_cast<size_t>(c) * dim + d] + noise(rng);
            }
            data.id_storage[c][v] = global_id;
            data.cluster_ids[c].push_back(global_id);
            ++global_id;
        }
    }

    data.clusters.resize(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        data.clusters[c].vectors =
            data.vectors.data() +
            static_cast<size_t>(data.cluster_ids[c][0]) * dim;
        data.clusters[c].ids = data.id_storage[c].data();
        data.clusters[c].count = cluster_size;
    }

    return data;
}

// Compute brute-force ground truth top-k for a set of queries.
static std::vector<std::vector<uint32_t>> ComputeGroundTruth(
    const float* queries, uint32_t num_queries,
    const float* database, uint32_t N, Dim dim, uint32_t top_k) {

    std::vector<std::vector<uint32_t>> gt(num_queries);
    for (uint32_t qi = 0; qi < num_queries; ++qi) {
        const float* q = queries + static_cast<size_t>(qi) * dim;
        std::vector<std::pair<float, uint32_t>> dists(N);
        for (uint32_t j = 0; j < N; ++j) {
            dists[j] = {simd::L2Sqr(q, database + static_cast<size_t>(j) * dim, dim), j};
        }
        std::partial_sort(dists.begin(), dists.begin() + top_k, dists.end());
        gt[qi].resize(top_k);
        for (uint32_t k = 0; k < top_k; ++k) {
            gt[qi][k] = dists[k].second;
        }
    }
    return gt;
}

// Generate random query vectors near the clusters.
static std::vector<float> GenerateQueries(uint32_t num_queries, Dim dim,
                                          const SyntheticData& data,
                                          uint64_t seed = 123) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> cluster_dist(0, data.nlist - 1);
    std::normal_distribution<float> noise(0.0f, 1.0f);

    std::vector<float> queries(static_cast<size_t>(num_queries) * dim);
    for (uint32_t qi = 0; qi < num_queries; ++qi) {
        uint32_t c = cluster_dist(rng);
        for (Dim d = 0; d < dim; ++d) {
            queries[static_cast<size_t>(qi) * dim + d] =
                data.centroids[static_cast<size_t>(c) * dim + d] + noise(rng);
        }
    }
    return queries;
}

TEST(CrcCalibratorTest, BasicCalibration) {
    // 4 clusters, 50 vectors each, 8D, 40 queries.
    auto data = GenerateData(4, 50, 8);
    auto queries = GenerateQueries(40, 8, data);
    auto gt = ComputeGroundTruth(queries.data(), 40, data.vectors.data(),
                                 data.N, 8, 10);

    CrcCalibrator::Config config;
    config.alpha = 0.1f;
    config.top_k = 10;
    config.seed = 42;

    auto [cal, eval] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters, gt);

    // Basic sanity checks.
    EXPECT_GT(cal.lamhat, 0.0f);
    EXPECT_LE(cal.lamhat, 1.0f);
    EXPECT_EQ(cal.kreg, 1u);
    EXPECT_LT(cal.d_min, cal.d_max);
    EXPECT_GT(eval.test_size, 0u);
}

TEST(CrcCalibratorTest, FnrBound) {
    // With well-separated clusters and enough queries, FNR should be <= alpha.
    auto data = GenerateData(4, 100, 8);
    auto queries = GenerateQueries(100, 8, data);
    auto gt = ComputeGroundTruth(queries.data(), 100, data.vectors.data(),
                                 data.N, 8, 5);

    CrcCalibrator::Config config;
    config.alpha = 0.2f;
    config.top_k = 5;
    config.seed = 42;

    auto [cal, eval] = CrcCalibrator::Calibrate(
        config, queries.data(), 100, 8,
        data.centroids.data(), 4, data.clusters, gt);

    // FNR on test set should be bounded by alpha (with some tolerance for
    // small sample variance).
    EXPECT_LE(eval.actual_fnr, config.alpha + 0.15f)
        << "FNR should be approximately bounded by alpha";
}

TEST(CrcCalibratorTest, Determinism) {
    auto data = GenerateData(4, 50, 8);
    auto queries = GenerateQueries(40, 8, data);
    auto gt = ComputeGroundTruth(queries.data(), 40, data.vectors.data(),
                                 data.N, 8, 10);

    CrcCalibrator::Config config;
    config.alpha = 0.1f;
    config.top_k = 10;
    config.seed = 42;

    auto [cal1, eval1] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters, gt);
    auto [cal2, eval2] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters, gt);

    EXPECT_FLOAT_EQ(cal1.lamhat, cal2.lamhat);
    EXPECT_EQ(cal1.kreg, cal2.kreg);
    EXPECT_FLOAT_EQ(cal1.reg_lambda, cal2.reg_lambda);
    EXPECT_FLOAT_EQ(cal1.d_min, cal2.d_min);
    EXPECT_FLOAT_EQ(cal1.d_max, cal2.d_max);
}

TEST(CrcCalibratorTest, LargerAlphaFewerProbes) {
    auto data = GenerateData(8, 50, 8);
    auto queries = GenerateQueries(80, 8, data);
    auto gt = ComputeGroundTruth(queries.data(), 80, data.vectors.data(),
                                 data.N, 8, 5);

    CrcCalibrator::Config config;
    config.top_k = 5;
    config.seed = 42;

    config.alpha = 0.05f;
    auto [cal_strict, eval_strict] = CrcCalibrator::Calibrate(
        config, queries.data(), 80, 8,
        data.centroids.data(), 8, data.clusters, gt);

    config.alpha = 0.3f;
    auto [cal_loose, eval_loose] = CrcCalibrator::Calibrate(
        config, queries.data(), 80, 8,
        data.centroids.data(), 8, data.clusters, gt);

    // Looser alpha should probe fewer or equal clusters on average.
    EXPECT_LE(eval_loose.avg_probed, eval_strict.avg_probed + 0.5f)
        << "Larger alpha should generally probe fewer clusters";
}

}  // namespace
}  // namespace index
}  // namespace vdb
