#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>
#include <vector>

#include "vdb/common/distance.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"

using namespace vdb;
using namespace vdb::query;
using namespace vdb::index;
namespace fs = std::filesystem;

class EarlyStopTest : public ::testing::Test {
 protected:
    static constexpr uint32_t N = 256;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;
    static constexpr uint32_t kTopK = 10;

    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_early_stop_test";
        fs::create_directories(test_dir_);

        std::mt19937 rng(42);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(N) * kDim);
        for (auto& v : vectors_) v = dist(rng);

        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 20;
        cfg.seed = 42;
        cfg.rabitq.c_factor = 5.75f;
        cfg.calibration_samples = 50;
        cfg.calibration_topk = kTopK;
        cfg.page_size = 1;

        IvfBuilder builder(cfg);
        auto s = builder.Build(vectors_.data(), N, kDim, test_dir_.string());
        ASSERT_TRUE(s.ok()) << s.message();

        index_ = std::make_unique<IvfIndex>();
        s = index_->Open(test_dir_.string());
        ASSERT_TRUE(s.ok()) << s.message();
    }

    void TearDown() override {
        index_.reset();
        fs::remove_all(test_dir_);
    }

    std::vector<std::pair<float, uint32_t>> BruteForceTopK(
        const float* query, uint32_t top_k) {
        std::vector<std::pair<float, uint32_t>> all;
        all.reserve(N);
        for (uint32_t i = 0; i < N; ++i) {
            float d = L2Sqr(query, vectors_.data() + static_cast<size_t>(i) * kDim, kDim);
            all.push_back({d, i});
        }
        std::partial_sort(all.begin(), all.begin() + top_k, all.end());
        all.resize(top_k);
        return all;
    }

    fs::path test_dir_;
    std::vector<float> vectors_;
    std::unique_ptr<IvfIndex> index_;
};

// Test: Early stop stats fields are correctly populated.
// With PreadFallbackReader, early stop relies on completions consumed during
// WaitAndPoll. Since PreadFallback prefetches all cluster blocks synchronously,
// WaitAndPoll may not consume vec completions, so early stop may not trigger.
// This test verifies stats consistency regardless.
TEST_F(EarlyStopTest, StatsConsistency) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNlist;

    OverlapScheduler scheduler(*index_, reader, config);

    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);
    EXPECT_NEAR(results[0].distance, 0.0f, 1e-4f);

    const auto& stats = results.stats();
    if (stats.early_stopped) {
        EXPECT_GT(stats.clusters_skipped, 0u);
    } else {
        EXPECT_EQ(stats.clusters_skipped, 0u);
    }
}

// Test: With nprobe=1, only 1 cluster is probed. The loop ends after 1
// iteration, so early stop cannot trigger (nothing to skip).
TEST_F(EarlyStopTest, NoEarlyStopWithSingleProbe) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = 1;

    OverlapScheduler scheduler(*index_, reader, config);

    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);

    EXPECT_FALSE(results.stats().early_stopped);
    EXPECT_EQ(results.stats().clusters_skipped, 0u);
}

// Test: With early_stop=false, all nprobe clusters are always probed,
// giving exact results matching brute force.
TEST_F(EarlyStopTest, DisabledGivesExactResults) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNlist;
    config.early_stop = false;

    OverlapScheduler scheduler(*index_, reader, config);

    for (uint32_t q = 0; q < 5; ++q) {
        const float* query = vectors_.data() + static_cast<size_t>(q) * kDim;
        auto results = scheduler.Search(query);
        auto ground_truth = BruteForceTopK(query, kTopK);

        ASSERT_EQ(results.size(), kTopK) << "Query " << q;
        EXPECT_FALSE(results.stats().early_stopped);

        for (uint32_t i = 0; i < kTopK; ++i) {
            EXPECT_NEAR(results[i].distance, ground_truth[i].first, 1e-4f)
                << "Query " << q << " rank " << i;
        }
    }
}
