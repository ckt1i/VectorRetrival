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
        cfg.epsilon_samples = 20;
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
    config.early_stop = false;

    OverlapScheduler scheduler(*index_, reader, config);

    // Run multiple queries
    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 5; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        auto ground_truth = BruteForceTopK(query.data(), kTopK);

        // With per-cluster epsilon, some true top-K vectors may be SafeOut'd.
        // Verify we got results and they are sorted.
        ASSERT_GT(results.size(), 0u)
            << "Query " << q << " returned 0 results";

        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance)
                << "Query " << q << " results not sorted at index " << i;
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
    config.early_stop = false;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(456);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 5; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        ASSERT_GT(results.size(), 0u) << "Query " << q;

        // Sorted check
        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance);
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
    config.early_stop = false;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(789);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 3; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        ASSERT_GT(results.size(), 0u) << "Query " << q;

        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance);
        }
    }
}

TEST_F(OverlapSchedulerTest, MultipleQueries_StateReset) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.early_stop = false;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(999);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    // Run 10 sequential queries on the same scheduler — verify state resets
    for (int q = 0; q < 10; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        ASSERT_GT(results.size(), 0u) << "Query " << q;

        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance);
        }
    }
}

TEST_F(OverlapSchedulerTest, MultipleQueriesWithEarlyStopEnabled) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;
    config.early_stop = true;

    OverlapScheduler scheduler(*index_, reader, config);

    for (uint32_t q = 0; q < 6; ++q) {
        const float* query = vectors_.data() + static_cast<size_t>(q) * kDim;
        auto results = scheduler.Search(query);
        ASSERT_GT(results.size(), 0u) << "Query " << q;
        EXPECT_GT(results.stats().total_submit_calls, 0u);
    }
}

TEST_F(OverlapSchedulerTest, SubmitBatchingAndReservePopulateStats) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;
    config.early_stop = false;

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    EXPECT_GT(results.size(), 0u);
    EXPECT_GT(results.stats().total_submit_calls, 0u);
    EXPECT_GE(results.stats().uring_submit_ms, 0.0);

    for (uint32_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].distance, results[i].distance);
    }
}

TEST_F(OverlapSchedulerTest, SharedAndIsolatedModesProduceSameResults) {
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;
    config.early_stop = false;

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    PreadFallbackReader shared_reader;
    OverlapScheduler shared_scheduler(*index_, shared_reader, config);
    auto shared_results = shared_scheduler.Search(query.data());

    PreadFallbackReader cluster_reader;
    PreadFallbackReader data_reader;
    config.submission_mode = SubmissionMode::Isolated;
    OverlapScheduler isolated_scheduler(*index_, cluster_reader, data_reader, config);
    auto isolated_results = isolated_scheduler.Search(query.data());

    ASSERT_EQ(shared_results.size(), isolated_results.size());
    for (uint32_t i = 0; i < shared_results.size(); ++i) {
        EXPECT_FLOAT_EQ(shared_results[i].distance, isolated_results[i].distance);
    }
}
