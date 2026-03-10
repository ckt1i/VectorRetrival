#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;

namespace fs = std::filesystem;

// ============================================================================
// Test fixture
// ============================================================================

class IvfBuilderTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_ivf_builder_test").string();
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    /// Generate N random Gaussian vectors (row-major, N × dim)
    static std::vector<float> GenerateVectors(uint32_t N, Dim dim,
                                               uint64_t seed = 42) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        std::vector<float> vecs(static_cast<size_t>(N) * dim);
        for (auto& v : vecs) v = dist(rng);
        return vecs;
    }

    std::string test_dir_;
};

// ============================================================================
// Basic build test
// ============================================================================

TEST_F(IvfBuilderTest, Build_Basic) {
    constexpr uint32_t N = 128;
    constexpr Dim dim = 64;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 10;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    auto s = builder.Build(vecs.data(), N, dim, test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    // Verify output files exist
    EXPECT_TRUE(fs::exists(test_dir_ + "/segment.meta"));
    EXPECT_TRUE(fs::exists(test_dir_ + "/centroids.bin"));
    EXPECT_TRUE(fs::exists(test_dir_ + "/rotation.bin"));

    // Verify per-cluster files
    for (uint32_t k = 0; k < nlist; ++k) {
        char clu_name[32], dat_name[32];
        std::snprintf(clu_name, sizeof(clu_name), "cluster_%04u.clu", k);
        std::snprintf(dat_name, sizeof(dat_name), "cluster_%04u.dat", k);
        EXPECT_TRUE(fs::exists(test_dir_ + "/" + clu_name))
            << "Missing " << clu_name;
        EXPECT_TRUE(fs::exists(test_dir_ + "/" + dat_name))
            << "Missing " << dat_name;
    }

    // Verify assignments cover all vectors
    EXPECT_EQ(builder.assignments().size(), N);
    for (auto a : builder.assignments()) {
        EXPECT_LT(a, nlist);
    }

    // Verify centroids shape
    EXPECT_EQ(builder.centroids().size(),
              static_cast<size_t>(nlist) * dim);

    // Verify calibrated d_k is positive
    EXPECT_GT(builder.calibrated_dk(), 0.0f);
}

// ============================================================================
// Assignment completeness: every vector is assigned to exactly one cluster
// ============================================================================

TEST_F(IvfBuilderTest, AssignmentCompleteness) {
    constexpr uint32_t N = 200;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 8;

    auto vecs = GenerateVectors(N, dim, 123);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 15;
    cfg.seed = 123;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());

    const auto& assignments = builder.assignments();
    ASSERT_EQ(assignments.size(), N);

    // Count assignments per cluster
    std::vector<uint32_t> counts(nlist, 0);
    for (auto a : assignments) {
        ASSERT_LT(a, nlist);
        counts[a]++;
    }

    // Every cluster should have at least 1 vector
    for (uint32_t k = 0; k < nlist; ++k) {
        EXPECT_GT(counts[k], 0u) << "Cluster " << k << " is empty";
    }

    // Total should equal N
    uint32_t total = 0;
    for (auto c : counts) total += c;
    EXPECT_EQ(total, N);
}

// ============================================================================
// Balanced clustering: with balance_factor, sizes should be more uniform
// ============================================================================

TEST_F(IvfBuilderTest, BalancedClustering) {
    constexpr uint32_t N = 256;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 8;

    auto vecs = GenerateVectors(N, dim, 999);

    // Build without balance
    IvfBuilderConfig cfg_unbal;
    cfg_unbal.nlist = nlist;
    cfg_unbal.max_iterations = 20;
    cfg_unbal.seed = 999;
    cfg_unbal.balance_factor = 0.0f;
    cfg_unbal.rabitq = {1, 64, 5.75f};
    cfg_unbal.calibration_samples = 10;
    cfg_unbal.calibration_topk = 5;
    cfg_unbal.page_size = 1;

    std::string dir_unbal = test_dir_ + "/unbalanced";
    IvfBuilder builder_unbal(cfg_unbal);
    ASSERT_TRUE(builder_unbal.Build(vecs.data(), N, dim, dir_unbal).ok());

    // Build with strong balance
    IvfBuilderConfig cfg_bal;
    cfg_bal.nlist = nlist;
    cfg_bal.max_iterations = 20;
    cfg_bal.seed = 999;
    cfg_bal.balance_factor = 0.3f;
    cfg_bal.rabitq = {1, 64, 5.75f};
    cfg_bal.calibration_samples = 10;
    cfg_bal.calibration_topk = 5;
    cfg_bal.page_size = 1;

    std::string dir_bal = test_dir_ + "/balanced";
    IvfBuilder builder_bal(cfg_bal);
    ASSERT_TRUE(builder_bal.Build(vecs.data(), N, dim, dir_bal).ok());

    // Compute standard deviation of cluster sizes
    auto compute_stddev = [&](const std::vector<uint32_t>& assignments) {
        std::vector<uint32_t> counts(nlist, 0);
        for (auto a : assignments) counts[a]++;
        double mean = static_cast<double>(N) / nlist;
        double var = 0.0;
        for (auto c : counts) {
            double diff = static_cast<double>(c) - mean;
            var += diff * diff;
        }
        return std::sqrt(var / nlist);
    };

    double stddev_bal = compute_stddev(builder_bal.assignments());

    // With balance_factor, the max cluster should respect capacity constraint
    const uint32_t max_cap = static_cast<uint32_t>(
        std::ceil(static_cast<double>(N) * (1.0 + cfg_bal.balance_factor) /
                  static_cast<double>(nlist)));

    std::vector<uint32_t> counts_bal(nlist, 0);
    for (auto a : builder_bal.assignments()) counts_bal[a]++;

    for (uint32_t k = 0; k < nlist; ++k) {
        EXPECT_LE(counts_bal[k], max_cap)
            << "Balanced cluster " << k << " exceeds max_cap=" << max_cap;
    }

    // Balanced std dev should be reasonably small
    double ideal = static_cast<double>(N) / nlist;
    EXPECT_LT(stddev_bal, ideal * 0.5)
        << "Balanced clustering has unexpectedly high variance";
}

// ============================================================================
// Roundtrip: build → open → verify nearest cluster contains the vector
// ============================================================================

TEST_F(IvfBuilderTest, BuildAndOpen_Roundtrip) {
    constexpr uint32_t N = 128;
    constexpr Dim dim = 64;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 10;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());

    // Open with IvfIndex
    IvfIndex idx;
    auto os = idx.Open(test_dir_);
    ASSERT_TRUE(os.ok()) << "Open failed: " << os.message();

    EXPECT_EQ(idx.nlist(), nlist);
    EXPECT_EQ(idx.dim(), dim);

    // For every vector, the nearest cluster from the builder should match
    // the nearest cluster from IvfIndex
    for (uint32_t i = 0; i < N; ++i) {
        const float* q = vecs.data() + static_cast<size_t>(i) * dim;
        auto nearest = idx.FindNearestClusters(q, 1);
        ASSERT_EQ(nearest.size(), 1u);
        // The builder's assignment should match (builder uses index k,
        // IvfIndex uses ClusterID which equals k)
        EXPECT_EQ(nearest[0], builder.assignments()[i])
            << "Mismatch for vector " << i;
    }
}

// ============================================================================
// Progress callback
// ============================================================================

TEST_F(IvfBuilderTest, ProgressCallback) {
    constexpr uint32_t N = 64;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 5;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 5;
    cfg.calibration_topk = 3;
    cfg.page_size = 1;

    uint32_t callback_count = 0;
    IvfBuilder builder(cfg);
    builder.SetProgressCallback([&](uint32_t idx, uint32_t total) {
        EXPECT_EQ(total, nlist);
        EXPECT_EQ(idx, callback_count);
        callback_count++;
    });

    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());
    EXPECT_EQ(callback_count, nlist);
}

// ============================================================================
// Invalid inputs
// ============================================================================

TEST_F(IvfBuilderTest, InvalidInputs) {
    IvfBuilderConfig cfg;
    cfg.nlist = 4;
    cfg.rabitq = {1, 64, 5.75f};

    IvfBuilder builder(cfg);

    // null vectors
    EXPECT_FALSE(builder.Build(nullptr, 100, 64, test_dir_).ok());

    // N = 0
    float dummy = 1.0f;
    EXPECT_FALSE(builder.Build(&dummy, 0, 64, test_dir_).ok());

    // dim = 0
    EXPECT_FALSE(builder.Build(&dummy, 100, 0, test_dir_).ok());

    // nlist > N
    IvfBuilderConfig cfg2;
    cfg2.nlist = 200;
    cfg2.rabitq = {1, 64, 5.75f};
    IvfBuilder builder2(cfg2);
    auto vecs = GenerateVectors(100, 64);
    EXPECT_FALSE(builder2.Build(vecs.data(), 100, 64, test_dir_).ok());
}
