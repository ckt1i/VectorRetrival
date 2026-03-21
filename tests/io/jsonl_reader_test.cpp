#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vdb/io/jsonl_reader.h"

using namespace vdb;
using namespace vdb::io;

namespace fs = std::filesystem;

static std::string CocoDir() {
    const char* env = std::getenv("VDB_COCO_1K_DIR");
    return env ? env : "/home/zcq/VDB/data/coco_1k";
}

// =============================================================================
// Basic tests
// =============================================================================

TEST(JsonlReaderTest, ReadCoco1k_Metadata) {
    const std::string path = CocoDir() + "/metadata.jsonl";
    if (!fs::exists(path)) GTEST_SKIP() << "coco_1k dataset not found at: " << path;

    uint32_t count = 0;
    std::string first_line;

    auto s = ReadJsonlLines(path, [&](uint32_t line_num, std::string_view line) {
        if (line_num == 0) {
            first_line = std::string(line);
        }
        count++;
    });

    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(count, 1000u);

    // First line should contain image_id: 139
    EXPECT_NE(first_line.find("\"image_id\""), std::string::npos);
    EXPECT_NE(first_line.find("139"), std::string::npos);
}

TEST(JsonlReaderTest, FileNotFound) {
    auto s = ReadJsonlLines("/tmp/nonexistent_file.jsonl",
                            [](uint32_t, std::string_view) {});
    EXPECT_FALSE(s.ok());
}

TEST(JsonlReaderTest, SkipsEmptyLines) {
    // Create a temp file with some empty lines
    const std::string path = "/tmp/vdb_test_jsonl_empty.jsonl";
    {
        std::ofstream f(path);
        f << "{\"a\": 1}\n";
        f << "\n";
        f << "   \n";
        f << "{\"b\": 2}\n";
    }

    uint32_t count = 0;
    auto s = ReadJsonlLines(path, [&](uint32_t, std::string_view) { count++; });
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(count, 2u);
    fs::remove(path);
}
