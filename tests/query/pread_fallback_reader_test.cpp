#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/query/async_reader.h"

using namespace vdb::query;
namespace fs = std::filesystem;

class PreadFallbackTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_pread_test";
        fs::create_directories(test_dir_);

        // Write a test file with known content
        test_file_ = (test_dir_ / "data.bin").string();
        std::ofstream f(test_file_, std::ios::binary);
        for (uint32_t i = 0; i < 256; ++i) {
            uint8_t byte = static_cast<uint8_t>(i);
            f.write(reinterpret_cast<const char*>(&byte), 1);
        }

        fd_ = ::open(test_file_.c_str(), O_RDONLY);
        ASSERT_GE(fd_, 0);
    }

    void TearDown() override {
        if (fd_ >= 0) ::close(fd_);
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
    std::string test_file_;
    int fd_ = -1;
};

TEST_F(PreadFallbackTest, SingleRead) {
    PreadFallbackReader reader;

    uint8_t buf[16];
    ASSERT_TRUE(reader.PrepRead(fd_, buf, 16, 0).ok());
    EXPECT_EQ(reader.Submit(), 1u);
    EXPECT_EQ(reader.InFlight(), 0u);

    IoCompletion comp;
    EXPECT_EQ(reader.Poll(&comp, 1), 1u);
    EXPECT_EQ(comp.buffer, buf);
    EXPECT_EQ(comp.result, 16);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i));
    }
}

TEST_F(PreadFallbackTest, MultipleReads) {
    PreadFallbackReader reader;

    uint8_t buf1[8], buf2[8];
    ASSERT_TRUE(reader.PrepRead(fd_, buf1, 8, 0).ok());
    ASSERT_TRUE(reader.PrepRead(fd_, buf2, 8, 100).ok());
    EXPECT_EQ(reader.Submit(), 2u);

    IoCompletion comps[2];
    EXPECT_EQ(reader.Poll(comps, 2), 2u);

    // First read: bytes 0-7
    EXPECT_EQ(comps[0].result, 8);
    EXPECT_EQ(buf1[0], 0);
    EXPECT_EQ(buf1[7], 7);

    // Second read: bytes 100-107
    EXPECT_EQ(comps[1].result, 8);
    EXPECT_EQ(buf2[0], 100);
    EXPECT_EQ(buf2[7], 107);
}

TEST_F(PreadFallbackTest, WaitAndPollWorks) {
    PreadFallbackReader reader;

    uint8_t buf[4];
    ASSERT_TRUE(reader.PrepRead(fd_, buf, 4, 10).ok());
    reader.Submit();

    IoCompletion comp;
    EXPECT_EQ(reader.WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.result, 4);
    EXPECT_EQ(buf[0], 10);
}

TEST_F(PreadFallbackTest, EmptySubmit) {
    PreadFallbackReader reader;
    EXPECT_EQ(reader.Submit(), 0u);

    IoCompletion comp;
    EXPECT_EQ(reader.Poll(&comp, 1), 0u);
}
