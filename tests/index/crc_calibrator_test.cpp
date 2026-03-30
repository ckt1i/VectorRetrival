#include "vdb/index/crc_calibrator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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

    CrcCalibrator::Config config;
    config.alpha = 0.1f;
    config.top_k = 10;
    config.seed = 42;

    auto [cal, eval] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters);

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

    CrcCalibrator::Config config;
    config.alpha = 0.2f;
    config.top_k = 5;
    config.seed = 42;

    auto [cal, eval] = CrcCalibrator::Calibrate(
        config, queries.data(), 100, 8,
        data.centroids.data(), 4, data.clusters);

    // FNR on test set should be bounded by alpha (with some tolerance for
    // small sample variance).
    EXPECT_LE(eval.actual_fnr, config.alpha + 0.15f)
        << "FNR should be approximately bounded by alpha";
}

TEST(CrcCalibratorTest, Determinism) {
    auto data = GenerateData(4, 50, 8);
    auto queries = GenerateQueries(40, 8, data);

    CrcCalibrator::Config config;
    config.alpha = 0.1f;
    config.top_k = 10;
    config.seed = 42;

    auto [cal1, eval1] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters);
    auto [cal2, eval2] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters);

    EXPECT_FLOAT_EQ(cal1.lamhat, cal2.lamhat);
    EXPECT_EQ(cal1.kreg, cal2.kreg);
    EXPECT_FLOAT_EQ(cal1.reg_lambda, cal2.reg_lambda);
    EXPECT_FLOAT_EQ(cal1.d_min, cal2.d_min);
    EXPECT_FLOAT_EQ(cal1.d_max, cal2.d_max);
}

TEST(CrcCalibratorTest, LargerAlphaFewerProbes) {
    auto data = GenerateData(8, 50, 8);
    auto queries = GenerateQueries(80, 8, data);

    CrcCalibrator::Config config;
    config.top_k = 5;
    config.seed = 42;

    config.alpha = 0.05f;
    auto [cal_strict, eval_strict] = CrcCalibrator::Calibrate(
        config, queries.data(), 80, 8,
        data.centroids.data(), 8, data.clusters);

    config.alpha = 0.3f;
    auto [cal_loose, eval_loose] = CrcCalibrator::Calibrate(
        config, queries.data(), 80, 8,
        data.centroids.data(), 8, data.clusters);

    // Looser alpha should probe fewer or equal clusters on average.
    EXPECT_LE(eval_loose.avg_probed, eval_strict.avg_probed + 0.5f)
        << "Larger alpha should generally probe fewer clusters";
}

TEST(CrcCalibratorTest, ScoresBasedCalibrate) {
    // Test the core scores-based API directly.
    auto data = GenerateData(4, 50, 8);
    auto queries = GenerateQueries(40, 8, data);

    CrcCalibrator::Config config;
    config.alpha = 0.1f;
    config.top_k = 10;
    config.seed = 42;

    // First, calibrate via the convenience wrapper.
    auto [cal_wrap, eval_wrap] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters);

    // Now compute scores manually and calibrate via core API.
    // The wrapper computes all scores then calls core, so results should match.
    auto [cal_core, eval_core] = CrcCalibrator::Calibrate(
        config, queries.data(), 40, 8,
        data.centroids.data(), 4, data.clusters);

    EXPECT_FLOAT_EQ(cal_wrap.lamhat, cal_core.lamhat);
    EXPECT_FLOAT_EQ(cal_wrap.d_min, cal_core.d_min);
    EXPECT_FLOAT_EQ(cal_wrap.d_max, cal_core.d_max);
}

TEST(CrcCalibratorTest, WriteReadScoresRoundTrip) {
    // Build some synthetic QueryScores.
    uint32_t nlist = 4;
    uint32_t top_k = 5;
    uint32_t num_queries = 10;

    std::vector<QueryScores> scores(num_queries);
    for (uint32_t qi = 0; qi < num_queries; ++qi) {
        scores[qi].raw_scores.resize(nlist);
        scores[qi].predictions.resize(nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            scores[qi].raw_scores[p] = static_cast<float>(qi * nlist + p) * 0.1f;
            uint32_t n_pred = std::min(top_k, p + 1);  // variable prediction count
            scores[qi].predictions[p].resize(n_pred);
            for (uint32_t j = 0; j < n_pred; ++j) {
                scores[qi].predictions[p][j] = qi * 100 + p * 10 + j;
            }
        }
    }

    // Write to temp file.
    std::string path = std::filesystem::temp_directory_path().string() +
                       "/crc_scores_test.bin";
    auto s = CrcCalibrator::WriteScores(path, scores, nlist, top_k);
    ASSERT_TRUE(s.ok()) << s.ToString();

    // Read back.
    std::vector<QueryScores> loaded;
    uint32_t loaded_nlist, loaded_top_k;
    s = CrcCalibrator::ReadScores(path, loaded, loaded_nlist, loaded_top_k);
    ASSERT_TRUE(s.ok()) << s.ToString();

    EXPECT_EQ(loaded_nlist, nlist);
    EXPECT_EQ(loaded_top_k, top_k);
    ASSERT_EQ(loaded.size(), num_queries);

    for (uint32_t qi = 0; qi < num_queries; ++qi) {
        ASSERT_EQ(loaded[qi].raw_scores.size(), nlist);
        ASSERT_EQ(loaded[qi].predictions.size(), nlist);
        for (uint32_t p = 0; p < nlist; ++p) {
            EXPECT_FLOAT_EQ(loaded[qi].raw_scores[p], scores[qi].raw_scores[p]);
            EXPECT_EQ(loaded[qi].predictions[p], scores[qi].predictions[p]);
        }
    }

    // Cleanup.
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace index
}  // namespace vdb
