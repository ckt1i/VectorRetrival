#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vdb/io/vecs_reader.h"

using namespace vdb;
using namespace vdb::io;

namespace fs = std::filesystem;

// =============================================================================
// Helper: write a small fvecs file
// =============================================================================
static void WriteFvecs(const std::string& path,
                       const std::vector<std::vector<float>>& rows) {
    std::ofstream f(path, std::ios::binary);
    for (const auto& row : rows) {
        int32_t dim = static_cast<int32_t>(row.size());
        f.write(reinterpret_cast<const char*>(&dim), 4);
        f.write(reinterpret_cast<const char*>(row.data()),
                dim * sizeof(float));
    }
}

// =============================================================================
// Helper: write a small ivecs file
// =============================================================================
static void WriteIvecs(const std::string& path,
                       const std::vector<std::vector<int32_t>>& rows) {
    std::ofstream f(path, std::ios::binary);
    for (const auto& row : rows) {
        int32_t dim = static_cast<int32_t>(row.size());
        f.write(reinterpret_cast<const char*>(&dim), 4);
        f.write(reinterpret_cast<const char*>(row.data()),
                dim * sizeof(int32_t));
    }
}

// =============================================================================
// LoadFvecs tests
// =============================================================================

TEST(VecsReaderTest, LoadFvecs_Small) {
    const std::string path = "/tmp/vdb_test_small.fvecs";
    WriteFvecs(path, {{1.0f, 2.0f, 3.0f},
                      {4.0f, 5.0f, 6.0f}});

    auto result = LoadFvecs(path);
    ASSERT_TRUE(result.ok()) << result.status().ToString();

    const auto& arr = result.value();
    EXPECT_EQ(arr.rows, 2u);
    EXPECT_EQ(arr.cols, 3u);
    EXPECT_EQ(arr.data.size(), 6u);
    EXPECT_FLOAT_EQ(arr.data[0], 1.0f);
    EXPECT_FLOAT_EQ(arr.data[3], 4.0f);
    EXPECT_FLOAT_EQ(arr.data[5], 6.0f);

    fs::remove(path);
}

TEST(VecsReaderTest, LoadFvecs_SingleRecord) {
    const std::string path = "/tmp/vdb_test_single.fvecs";
    WriteFvecs(path, {{1.5f, -2.5f}});

    auto result = LoadFvecs(path);
    ASSERT_TRUE(result.ok());

    const auto& arr = result.value();
    EXPECT_EQ(arr.rows, 1u);
    EXPECT_EQ(arr.cols, 2u);
    EXPECT_FLOAT_EQ(arr.data[0], 1.5f);
    EXPECT_FLOAT_EQ(arr.data[1], -2.5f);

    fs::remove(path);
}

TEST(VecsReaderTest, LoadFvecs_NonexistentFile) {
    auto result = LoadFvecs("/tmp/nonexistent_file.fvecs");
    EXPECT_FALSE(result.ok());
}

TEST(VecsReaderTest, LoadFvecs_TruncatedFile) {
    const std::string path = "/tmp/vdb_test_trunc.fvecs";
    {
        std::ofstream f(path, std::ios::binary);
        int32_t dim = 3;
        f.write(reinterpret_cast<const char*>(&dim), 4);
        float val = 1.0f;
        f.write(reinterpret_cast<const char*>(&val), sizeof(float));
        // Only wrote 1 float instead of 3 — file size won't be multiple of record_size
    }

    auto result = LoadFvecs(path);
    EXPECT_FALSE(result.ok());

    fs::remove(path);
}

// =============================================================================
// LoadIvecs tests
// =============================================================================

TEST(VecsReaderTest, LoadIvecs_Small) {
    const std::string path = "/tmp/vdb_test_small.ivecs";
    WriteIvecs(path, {{10, 20, 30},
                      {40, 50, 60}});

    auto result = LoadIvecs(path);
    ASSERT_TRUE(result.ok()) << result.status().ToString();

    const auto& arr = result.value();
    EXPECT_EQ(arr.rows, 2u);
    EXPECT_EQ(arr.cols, 3u);
    EXPECT_EQ(arr.data.size(), 6u);
    EXPECT_EQ(arr.data[0], 10);
    EXPECT_EQ(arr.data[3], 40);
    EXPECT_EQ(arr.data[5], 60);

    fs::remove(path);
}

TEST(VecsReaderTest, LoadIvecs_NonexistentFile) {
    auto result = LoadIvecs("/tmp/nonexistent_file.ivecs");
    EXPECT_FALSE(result.ok());
}

// =============================================================================
// LoadVectors dispatch tests
// =============================================================================

TEST(VecsReaderTest, LoadVectors_Fvecs) {
    const std::string path = "/tmp/vdb_test_dispatch.fvecs";
    WriteFvecs(path, {{1.0f, 2.0f}});

    auto result = LoadVectors(path);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().rows, 1u);
    EXPECT_EQ(result.value().cols, 2u);

    fs::remove(path);
}

TEST(VecsReaderTest, LoadVectors_UnsupportedExtension) {
    auto result = LoadVectors("/tmp/test.csv");
    EXPECT_FALSE(result.ok());
}

// =============================================================================
// Real dataset tests (skipped if not present)
// =============================================================================

TEST(VecsReaderTest, LoadFvecs_DEEP1M) {
    const std::string path = "/home/zcq/VDB/data/deep1m/deep1m_base.fvecs";
    if (!fs::exists(path)) GTEST_SKIP() << "DEEP1M base not found at: " << path;

    auto result = LoadFvecs(path);
    ASSERT_TRUE(result.ok()) << result.status().ToString();

    const auto& arr = result.value();
    EXPECT_EQ(arr.rows, 1000000u);
    EXPECT_EQ(arr.cols, 96u);

    // Sanity: values should be finite
    for (uint32_t i = 0; i < 10; ++i) {
        EXPECT_TRUE(std::isfinite(arr.data[i]));
    }
}

TEST(VecsReaderTest, LoadIvecs_DEEP1M_GT) {
    const std::string path = "/home/zcq/VDB/data/deep1m/deep1m_groundtruth.ivecs";
    if (!fs::exists(path)) GTEST_SKIP() << "DEEP1M GT not found at: " << path;

    auto result = LoadIvecs(path);
    ASSERT_TRUE(result.ok()) << result.status().ToString();

    const auto& arr = result.value();
    EXPECT_GT(arr.rows, 0u);
    EXPECT_GT(arr.cols, 0u);
}
