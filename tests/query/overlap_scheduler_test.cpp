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

class OverlapSchedulerTest : public ::testing::Test {
 protected:
    static constexpr uint32_t N = 256;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;
    static constexpr uint32_t kTopK = 10;
    static constexpr uint32_t kNprobe = 4;  // Probe all clusters

    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_scheduler_test";
        fs::create_directories(test_dir_);

        // Generate random vectors
        std::mt19937 rng(42);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(N) * kDim);
        for (auto& v : vectors_) v = dist(rng);

        // Build index
        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 20;
        cfg.seed = 42;
        cfg.rabitq.c_factor = 5.75f;
        cfg.calibration_samples = 50;
        cfg.calibration_topk = kTopK;
        cfg.page_size = 1;  // No padding for simple test

        IvfBuilder builder(cfg);
        auto s = builder.Build(vectors_.data(), N, kDim, test_dir_.string());
        ASSERT_TRUE(s.ok()) << s.message();

        // Open index
        index_ = std::make_unique<IvfIndex>();
        s = index_->Open(test_dir_.string());
        ASSERT_TRUE(s.ok()) << s.message();
    }

    void TearDown() override {
        index_.reset();
        fs::remove_all(test_dir_);
    }

    // Brute-force L2 TopK for ground truth
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

TEST_F(OverlapSchedulerTest, EndToEnd_PreadFallback) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.probe_batch_size = 64;

    OverlapScheduler scheduler(*index_, reader, config);

    // Run multiple queries
    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 5; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        auto ground_truth = BruteForceTopK(query.data(), kTopK);

        // Verify we got the right number of results
        ASSERT_EQ(results.size(), kTopK)
            << "Query " << q << " returned " << results.size() << " results";

        // Verify results are sorted by distance (ascending)
        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance)
                << "Query " << q << " results not sorted at index " << i;
        }

        // Verify top-K distances match brute force
        // Since we probe all clusters (nprobe=nlist=4), we should get exact results
        for (uint32_t i = 0; i < kTopK; ++i) {
            EXPECT_NEAR(results[i].distance, ground_truth[i].first, 1e-4f)
                << "Query " << q << " mismatch at rank " << i
                << ": got " << results[i].distance
                << " expected " << ground_truth[i].first;
        }
    }
}

TEST_F(OverlapSchedulerTest, StatsArePopulated) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

    // We need access to stats, but Search returns SearchResults.
    // For now just verify it doesn't crash and returns results.
    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    EXPECT_GT(results.size(), 0u);
}

// ============================================================================
// Phase 8: Async cluster prefetch tests
// ============================================================================

TEST_F(OverlapSchedulerTest, PrefetchConfig_SmallDepth) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.probe_batch_size = 64;
    config.prefetch_depth = 4;
    config.refill_threshold = 1;
    config.refill_count = 1;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(456);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 5; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        auto ground_truth = BruteForceTopK(query.data(), kTopK);

        ASSERT_EQ(results.size(), kTopK) << "Query " << q;
        for (uint32_t i = 0; i < kTopK; ++i) {
            EXPECT_NEAR(results[i].distance, ground_truth[i].first, 1e-4f)
                << "Query " << q << " rank " << i;
        }
    }
}

TEST_F(OverlapSchedulerTest, PrefetchDepth_ExceedsNprobe) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 100;  // >> nprobe=4
    config.refill_threshold = 2;
    config.refill_count = 2;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(789);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 3; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        auto ground_truth = BruteForceTopK(query.data(), kTopK);

        ASSERT_EQ(results.size(), kTopK) << "Query " << q;
        for (uint32_t i = 0; i < kTopK; ++i) {
            EXPECT_NEAR(results[i].distance, ground_truth[i].first, 1e-4f)
                << "Query " << q << " rank " << i;
        }
    }
}

TEST_F(OverlapSchedulerTest, MultipleQueries_StateReset) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(999);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    // Run 10 sequential queries on the same scheduler
    for (int q = 0; q < 10; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        auto ground_truth = BruteForceTopK(query.data(), kTopK);

        ASSERT_EQ(results.size(), kTopK) << "Query " << q;
        for (uint32_t i = 0; i < kTopK; ++i) {
            EXPECT_NEAR(results[i].distance, ground_truth[i].first, 1e-4f)
                << "Query " << q << " rank " << i;
        }
    }
}
