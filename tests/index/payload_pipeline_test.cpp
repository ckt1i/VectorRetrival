#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "vdb/common/distance.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::query;

namespace fs = std::filesystem;

// ============================================================================
// Test fixture
// ============================================================================

class PayloadPipelineTest : public ::testing::Test {
 protected:
    static constexpr uint32_t N = 128;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;

    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_payload_test").string();
        fs::create_directories(test_dir_);

        // Generate random vectors
        std::mt19937 rng(42);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(N) * kDim);
        for (auto& v : vectors_) v = dist(rng);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    IvfBuilderConfig MakeConfig() {
        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 10;
        cfg.seed = 42;
        cfg.rabitq = {1, 64, 5.75f};
        cfg.calibration_samples = 10;
        cfg.calibration_topk = 5;
        cfg.page_size = 1;
        return cfg;
    }

    std::string test_dir_;
    std::vector<float> vectors_;
};

// ============================================================================
// Test: Build with PayloadFn → Open → Search → verify payload
// ============================================================================

TEST_F(PayloadPipelineTest, BuildWithPayload_Roundtrip) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64, false},
        {1, "data", DType::BYTES, false},
    };

    IvfBuilder builder(cfg);

    // PayloadFn: each vector gets its index as id and a synthetic data blob
    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        std::string blob = "payload_data_" + std::to_string(vec_index);
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::Bytes(std::move(blob))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    // Open index
    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    // Verify payload schemas were persisted and restored
    ASSERT_EQ(index.payload_schemas().size(), 2u);
    EXPECT_EQ(index.payload_schemas()[0].name, "id");
    EXPECT_EQ(index.payload_schemas()[0].dtype, DType::INT64);
    EXPECT_EQ(index.payload_schemas()[1].name, "data");
    EXPECT_EQ(index.payload_schemas()[1].dtype, DType::BYTES);

    // Search and verify payloads
    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;  // Probe all clusters
    search_cfg.safein_all_threshold = 0;  // Always read full record for SafeIn

    OverlapScheduler scheduler(index, reader, search_cfg);

    // Use vector 0 as the query → its own record should be the closest result
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);

    // The closest result should have distance ≈ 0 (the query itself)
    EXPECT_NEAR(results[0].distance, 0.0f, 1e-3f);

    // Verify payload was read back correctly
    ASSERT_EQ(results[0].payload.size(), 2u);
    EXPECT_EQ(results[0].payload[0].dtype, DType::INT64);

    // The id in payload[0] should correspond to some original vector index
    int64_t result_id = results[0].payload[0].fixed.i64;
    EXPECT_GE(result_id, 0);
    EXPECT_LT(result_id, static_cast<int64_t>(N));

    // The data in payload[1] should match the expected synthetic blob
    std::string expected_blob = "payload_data_" + std::to_string(result_id);
    EXPECT_EQ(results[0].payload[1].dtype, DType::BYTES);
    EXPECT_EQ(results[0].payload[1].var_data, expected_blob);
}

// ============================================================================
// Test: Build without PayloadFn → backward compat (empty payload)
// ============================================================================

TEST_F(PayloadPipelineTest, BuildWithoutPayload_BackwardCompat) {
    auto cfg = MakeConfig();
    // No payload_schemas, no PayloadFn

    IvfBuilder builder(cfg);
    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    // No payload schemas
    EXPECT_TRUE(index.payload_schemas().empty());

    // Search still works
    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;

    OverlapScheduler scheduler(index, reader, search_cfg);
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);

    // Payload should be empty
    EXPECT_TRUE(results[0].payload.empty());
}

// ============================================================================
// Test: Payload schemas round-trip through segment.meta
// ============================================================================

TEST_F(PayloadPipelineTest, SegmentMeta_PayloadSchemas_Roundtrip) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64,  false},
        {1, "text", DType::STRING, true},
    };

    IvfBuilder builder(cfg);

    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::String("item_" + std::to_string(vec_index))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    const auto& schemas = index.payload_schemas();
    ASSERT_EQ(schemas.size(), 2u);

    EXPECT_EQ(schemas[0].id, 0u);
    EXPECT_EQ(schemas[0].name, "id");
    EXPECT_EQ(schemas[0].dtype, DType::INT64);
    EXPECT_EQ(schemas[0].nullable, false);

    EXPECT_EQ(schemas[1].id, 1u);
    EXPECT_EQ(schemas[1].name, "text");
    EXPECT_EQ(schemas[1].dtype, DType::STRING);
    EXPECT_EQ(schemas[1].nullable, true);
}

// ============================================================================
// Test: Build with PayloadFn returning STRING payload
// ============================================================================

TEST_F(PayloadPipelineTest, BuildWithStringPayload) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64,  false},
        {1, "text", DType::STRING, false},
    };

    IvfBuilder builder(cfg);

    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::String("text_" + std::to_string(vec_index))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;
    search_cfg.safein_all_threshold = 0;

    OverlapScheduler scheduler(index, reader, search_cfg);
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);
    ASSERT_EQ(results[0].payload.size(), 2u);

    int64_t result_id = results[0].payload[0].fixed.i64;
    EXPECT_EQ(results[0].payload[1].dtype, DType::STRING);
    EXPECT_EQ(results[0].payload[1].var_data, "text_" + std::to_string(result_id));
}
