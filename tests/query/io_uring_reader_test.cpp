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

class IoUringReaderTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_iouring_test";
        fs::create_directories(test_dir_);

        // Write test file: 1024 bytes, byte[i] = i % 256
        test_file_ = (test_dir_ / "data.bin").string();
        std::ofstream f(test_file_, std::ios::binary);
        for (uint32_t i = 0; i < 1024; ++i) {
            uint8_t byte = static_cast<uint8_t>(i % 256);
            f.write(reinterpret_cast<const char*>(&byte), 1);
        }

        fd_ = ::open(test_file_.c_str(), O_RDONLY);
        ASSERT_GE(fd_, 0);

        reader_ = std::make_unique<IoUringReader>();
        auto s = reader_->Init(64, 256);
        if (!s.ok()) {
            GTEST_SKIP() << "io_uring not available: " << s.message();
        }
    }

    void TearDown() override {
        reader_.reset();
        if (fd_ >= 0) ::close(fd_);
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
    std::string test_file_;
    int fd_ = -1;
    std::unique_ptr<IoUringReader> reader_;
};

TEST_F(IoUringReaderTest, SingleRead) {
    uint8_t buf[16];
    ASSERT_TRUE(reader_->PrepRead(fd_, buf, 16, 0).ok());
    EXPECT_EQ(reader_->Submit(), 1u);

    IoCompletion comp;
    EXPECT_EQ(reader_->WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.buffer, buf);
    EXPECT_EQ(comp.result, 16);
    EXPECT_EQ(reader_->InFlight(), 0u);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i));
    }
}

TEST_F(IoUringReaderTest, MultipleReads) {
    uint8_t buf1[8], buf2[8], buf3[8];
    ASSERT_TRUE(reader_->PrepRead(fd_, buf1, 8, 0).ok());
    ASSERT_TRUE(reader_->PrepRead(fd_, buf2, 8, 100).ok());
    ASSERT_TRUE(reader_->PrepRead(fd_, buf3, 8, 200).ok());
    EXPECT_EQ(reader_->Submit(), 3u);
    EXPECT_EQ(reader_->InFlight(), 3u);

    IoCompletion comps[3];
    uint32_t total = 0;
    while (total < 3) {
        total += reader_->WaitAndPoll(comps + total, 3 - total);
    }
    EXPECT_EQ(reader_->InFlight(), 0u);

    // Verify all completions arrived (order may vary)
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(comps[i].result, 8);
    }
}

TEST_F(IoUringReaderTest, EmptySubmit) {
    EXPECT_EQ(reader_->Submit(), 0u);
    EXPECT_EQ(reader_->InFlight(), 0u);
}

TEST_F(IoUringReaderTest, PollNonBlocking) {
    IoCompletion comp;
    EXPECT_EQ(reader_->Poll(&comp, 1), 0u);
}
