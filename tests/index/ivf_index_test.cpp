#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
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
// Test fixture — builds a small IVF index, then opens it with IvfIndex
// ============================================================================

class IvfIndexTest : public ::testing::Test {
 protected:
    static constexpr uint32_t kN = 256;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;
    static constexpr uint64_t kSeed = 42;

    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_ivf_index_test").string();
        fs::create_directories(test_dir_);

        // Generate random vectors
        std::mt19937 rng(kSeed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(kN) * kDim);
        for (auto& v : vectors_) v = dist(rng);

        // Build the index
        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 10;
        cfg.tolerance = 1e-4f;
        cfg.seed = kSeed;
        cfg.balance_factor = 0.0f;
        cfg.rabitq = {1, 64, 5.75f};
        cfg.calibration_samples = 10;
        cfg.calibration_topk = 5;
        cfg.calibration_percentile = 0.95f;
        cfg.page_size = 1;

        IvfBuilder builder(cfg);
        auto s = builder.Build(vectors_.data(), kN, kDim, test_dir_);
        ASSERT_TRUE(s.ok()) << s.message();
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string test_dir_;
    std::vector<float> vectors_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(IvfIndexTest, OpenSucceeds) {
    IvfIndex idx;
    auto s = idx.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(idx.nlist(), kNlist);
    EXPECT_EQ(idx.dim(), kDim);
    EXPECT_EQ(idx.cluster_ids().size(), kNlist);
}

TEST_F(IvfIndexTest, FindNearestClusters_Basic) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // Pick first vector as query
    const float* query = vectors_.data();

    // nprobe = 1: should return exactly 1 cluster
    auto result = idx.FindNearestClusters(query, 1);
    EXPECT_EQ(result.size(), 1u);

    // nprobe = nlist: should return all clusters
    auto result_all = idx.FindNearestClusters(query, kNlist);
    EXPECT_EQ(result_all.size(), kNlist);

    // The nearest cluster from nprobe=1 should also be the first in nprobe=nlist
    EXPECT_EQ(result[0], result_all[0]);
}

TEST_F(IvfIndexTest, FindNearestClusters_SortedByDistance) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    const float* query = vectors_.data();
    auto result = idx.FindNearestClusters(query, kNlist);

    // Verify results are sorted by distance (ascending)
    for (uint32_t i = 0; i < kNlist; ++i) {
        // Find centroid index for this cluster ID
        uint32_t cidx_i = 0;
        for (uint32_t c = 0; c < kNlist; ++c) {
            if (idx.cluster_ids()[c] == result[i]) {
                cidx_i = c;
                break;
            }
        }
        if (i > 0) {
            uint32_t cidx_prev = 0;
            for (uint32_t c = 0; c < kNlist; ++c) {
                if (idx.cluster_ids()[c] == result[i - 1]) {
                    cidx_prev = c;
                    break;
                }
            }
            float d_prev = simd::L2Sqr(query, idx.centroid(cidx_prev), kDim);
            float d_curr = simd::L2Sqr(query, idx.centroid(cidx_i), kDim);
            EXPECT_LE(d_prev, d_curr + 1e-6f)
                << "Clusters not sorted by distance at position " << i;
        }
    }
}

TEST_F(IvfIndexTest, ConANN_LoadedCorrectly) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // epsilon and d_k should be positive after calibration
    EXPECT_GT(idx.conann().epsilon(), 0.0f);
    EXPECT_GT(idx.conann().d_k(), 0.0f);

    // tau_in < d_k < tau_out
    EXPECT_LT(idx.conann().tau_in(), idx.conann().d_k());
    EXPECT_GT(idx.conann().tau_out(), idx.conann().d_k());
}

TEST_F(IvfIndexTest, SegmentAccessible) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // Should be able to access clusters via the segment
    for (auto cid : idx.cluster_ids()) {
        auto reader = idx.segment().GetCluster(cid);
        ASSERT_NE(reader, nullptr) << "Cluster " << cid << " not found";
        EXPECT_EQ(reader->dim(), kDim);
        EXPECT_GT(reader->num_records(), 0u);
    }
}

TEST_F(IvfIndexTest, NprobeClamped) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // nprobe > nlist should be clamped
    auto result = idx.FindNearestClusters(vectors_.data(), kNlist + 100);
    EXPECT_EQ(result.size(), kNlist);
}

TEST_F(IvfIndexTest, NprobeZero_ReturnsEmpty) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    auto result = idx.FindNearestClusters(vectors_.data(), 0);
    EXPECT_TRUE(result.empty());
}

TEST_F(IvfIndexTest, OpenInvalidDir_Fails) {
    IvfIndex idx;
    auto s = idx.Open("/tmp/nonexistent_ivf_dir_12345");
    EXPECT_FALSE(s.ok());
}
