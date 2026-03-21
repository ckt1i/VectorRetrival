#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "vdb/io/npy_reader.h"

using namespace vdb;
using namespace vdb::io;

namespace fs = std::filesystem;

// Path to coco_1k dataset (set via environment or default)
static std::string CocoDir() {
    const char* env = std::getenv("VDB_COCO_1K_DIR");
    return env ? env : "/home/zcq/VDB/data/coco_1k";
}

// =============================================================================
// Float32 tests
// =============================================================================

TEST(NpyReaderTest, LoadFloat32_Coco1k) {
    const std::string path = CocoDir() + "/image_embeddings.npy";
    if (!fs::exists(path)) GTEST_SKIP() << "coco_1k dataset not found at: " << path;

    auto result = LoadNpyFloat32(path);
    ASSERT_TRUE(result.ok()) << result.status().message();

    const auto& arr = result.value();
    EXPECT_EQ(arr.rows, 1000u);
    EXPECT_EQ(arr.cols, 512u);
    EXPECT_EQ(arr.data.size(), 1000u * 512u);

    // Sanity: values should be finite floats
    for (uint32_t i = 0; i < 10; ++i) {
        EXPECT_TRUE(std::isfinite(arr.data[i])) << "data[" << i << "] is not finite";
    }
}

TEST(NpyReaderTest, LoadFloat32_InvalidFile) {
    auto result = LoadNpyFloat32("/tmp/nonexistent_file.npy");
    EXPECT_FALSE(result.ok());
}

TEST(NpyReaderTest, LoadFloat32_CorruptMagic) {
    // Write a file with invalid magic
    const std::string path = "/tmp/vdb_test_bad_magic.npy";
    {
        std::ofstream f(path, std::ios::binary);
        f << "NOT_NPY";
    }
    auto result = LoadNpyFloat32(path);
    EXPECT_FALSE(result.ok());
    fs::remove(path);
}

// =============================================================================
// Int64 tests
// =============================================================================

TEST(NpyReaderTest, LoadInt64_Coco1k) {
    const std::string path = CocoDir() + "/image_ids.npy";
    if (!fs::exists(path)) GTEST_SKIP() << "coco_1k dataset not found at: " << path;

    auto result = LoadNpyInt64(path);
    ASSERT_TRUE(result.ok()) << result.status().message();

    const auto& arr = result.value();
    EXPECT_EQ(arr.count, 1000u);
    EXPECT_EQ(arr.data.size(), 1000u);

    // First id should be 139 (from coco_1k dataset)
    EXPECT_EQ(arr.data[0], 139);
}

TEST(NpyReaderTest, LoadInt64_WrongDtype) {
    // Try loading a float32 file as int64 — should fail
    const std::string path = CocoDir() + "/image_embeddings.npy";
    if (!fs::exists(path)) GTEST_SKIP() << "coco_1k dataset not found at: " << path;

    auto result = LoadNpyInt64(path);
    EXPECT_FALSE(result.ok());
}
