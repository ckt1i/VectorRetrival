#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/data_file_reader.h"
#include "vdb/storage/data_file_writer.h"

using namespace vdb;
using namespace vdb::storage;

namespace fs = std::filesystem;

class DataFileTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_datafile_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string TestPath(const std::string& name) {
        return (test_dir_ / name).string();
    }

    fs::path test_dir_;
};

// ============================================================================
// DataFileWriter tests
// ============================================================================

TEST_F(DataFileTest, Writer_OpenClose) {
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 64).ok());
    EXPECT_EQ(writer.num_records(), 0u);
    ASSERT_TRUE(writer.Finalize().ok());
}

TEST_F(DataFileTest, Writer_SingleRecord_NoPayload) {
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 4).ok());

    float vec[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec, {}, addr).ok());

    EXPECT_EQ(addr.offset, 0u);
    EXPECT_EQ(addr.size, 4 * sizeof(float));
    EXPECT_EQ(writer.num_records(), 1u);

    ASSERT_TRUE(writer.Finalize().ok());
}

TEST_F(DataFileTest, Writer_MultipleRecords) {
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 8).ok());

    std::vector<AddressEntry> addrs;
    for (int i = 0; i < 10; ++i) {
        std::vector<float> vec(8, static_cast<float>(i));
        AddressEntry addr;
        ASSERT_TRUE(writer.WriteRecord(vec.data(), {}, addr).ok());
        addrs.push_back(addr);
    }

    EXPECT_EQ(writer.num_records(), 10u);

    // Offsets should be monotonically increasing
    for (int i = 1; i < 10; ++i) {
        EXPECT_GT(addrs[i].offset, addrs[i - 1].offset);
    }

    // All records same size (8 floats, no payload)
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(addrs[i].size, 8u * sizeof(float));
    }

    ASSERT_TRUE(writer.Finalize().ok());
}

TEST_F(DataFileTest, Writer_FixedPayload) {
    std::vector<ColumnSchema> schemas = {
        {0, "id", DType::UINT32},
        {1, "score", DType::FLOAT32},
    };

    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 4, schemas).ok());

    float vec[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<Datum> payload = {
        Datum::UInt32(42),
        Datum::Float32(3.14f),
    };

    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec, payload, addr).ok());

    EXPECT_EQ(addr.offset, 0u);
    // 4 floats + 1 uint32 + 1 float = 4*4 + 4 + 4 = 24
    EXPECT_EQ(addr.size, 24u);

    ASSERT_TRUE(writer.Finalize().ok());
}

TEST_F(DataFileTest, Writer_VarLengthPayload) {
    std::vector<ColumnSchema> schemas = {
        {0, "name", DType::STRING},
    };

    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 2, schemas).ok());

    float vec[2] = {1.0f, 2.0f};
    std::vector<Datum> payload = {Datum::String("hello")};

    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec, payload, addr).ok());

    // 2 floats + uint32(5) + "hello" = 8 + 4 + 5 = 17
    EXPECT_EQ(addr.size, 17u);

    ASSERT_TRUE(writer.Finalize().ok());
}

TEST_F(DataFileTest, Writer_PayloadSizeMismatch) {
    std::vector<ColumnSchema> schemas = {
        {0, "id", DType::UINT32},
    };

    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 4, schemas).ok());

    float vec[4] = {1, 2, 3, 4};
    // Empty payload but schema expects 1 column
    AddressEntry addr;
    auto s = writer.WriteRecord(vec, {}, addr);
    EXPECT_FALSE(s.ok());
}

TEST_F(DataFileTest, Writer_DoubleFinalize) {
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("test.dat"), 0, 4).ok());
    ASSERT_TRUE(writer.Finalize().ok());
    EXPECT_FALSE(writer.Finalize().ok());
}

// ============================================================================
// DataFileReader tests
// ============================================================================

TEST_F(DataFileTest, Reader_ReadRecord_NoPayload) {
    std::string path = TestPath("test.dat");

    // Write
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, 4).ok());

    float vec_in[4] = {1.5f, 2.5f, 3.5f, 4.5f};
    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec_in, {}, addr).ok());
    ASSERT_TRUE(writer.Finalize().ok());

    // Read
    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, 4).ok());

    float vec_out[4] = {0};
    std::vector<Datum> payload_out;
    ASSERT_TRUE(reader.ReadRecord(addr, vec_out, payload_out).ok());

    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(vec_out[i], vec_in[i]);
    }
    EXPECT_TRUE(payload_out.empty());
}

TEST_F(DataFileTest, Reader_ReadRecord_FixedPayload) {
    std::string path = TestPath("test.dat");
    std::vector<ColumnSchema> schemas = {
        {0, "id", DType::UINT32},
        {1, "value", DType::FLOAT64},
    };

    // Write
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, 2, schemas).ok());

    float vec_in[2] = {10.0f, 20.0f};
    std::vector<Datum> payload_in = {
        Datum::UInt32(999),
        Datum::Float64(2.718),
    };

    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec_in, payload_in, addr).ok());
    ASSERT_TRUE(writer.Finalize().ok());

    // Read
    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, 2, schemas).ok());

    float vec_out[2];
    std::vector<Datum> payload_out;
    ASSERT_TRUE(reader.ReadRecord(addr, vec_out, payload_out).ok());

    EXPECT_FLOAT_EQ(vec_out[0], 10.0f);
    EXPECT_FLOAT_EQ(vec_out[1], 20.0f);
    ASSERT_EQ(payload_out.size(), 2u);
    EXPECT_EQ(payload_out[0].fixed.u32, 999u);
    EXPECT_DOUBLE_EQ(payload_out[1].fixed.f64, 2.718);
}

TEST_F(DataFileTest, Reader_ReadRecord_VarLengthPayload) {
    std::string path = TestPath("test.dat");
    std::vector<ColumnSchema> schemas = {
        {0, "label", DType::STRING},
    };

    // Write
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, 2, schemas).ok());

    float vec_in[2] = {1.0f, 2.0f};
    std::vector<Datum> payload_in = {Datum::String("world")};

    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec_in, payload_in, addr).ok());
    ASSERT_TRUE(writer.Finalize().ok());

    // Read
    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, 2, schemas).ok());

    float vec_out[2];
    std::vector<Datum> payload_out;
    ASSERT_TRUE(reader.ReadRecord(addr, vec_out, payload_out).ok());

    ASSERT_EQ(payload_out.size(), 1u);
    EXPECT_EQ(payload_out[0].var_data, "world");
}

TEST_F(DataFileTest, Reader_MultipleRecords) {
    std::string path = TestPath("test.dat");
    const int N = 50;
    const Dim dim = 16;

    // Write
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, dim).ok());

    std::vector<AddressEntry> addrs;
    for (int i = 0; i < N; ++i) {
        std::vector<float> vec(dim, static_cast<float>(i));
        AddressEntry addr;
        ASSERT_TRUE(writer.WriteRecord(vec.data(), {}, addr).ok());
        addrs.push_back(addr);
    }
    ASSERT_TRUE(writer.Finalize().ok());

    // Read back in reverse order
    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, dim).ok());

    for (int i = N - 1; i >= 0; --i) {
        std::vector<float> vec(dim);
        std::vector<Datum> payload;
        ASSERT_TRUE(reader.ReadRecord(addrs[i], vec.data(), payload).ok());

        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(vec[d], static_cast<float>(i));
        }
    }
}

TEST_F(DataFileTest, Reader_ReadVector) {
    std::string path = TestPath("test.dat");
    std::vector<ColumnSchema> schemas = {{0, "id", DType::UINT32}};

    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, 4, schemas).ok());

    float vec_in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec_in, {Datum::UInt32(1)}, addr).ok());
    ASSERT_TRUE(writer.Finalize().ok());

    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, 4, schemas).ok());

    // ReadVector should only read the vector, not payload
    float vec_out[4];
    ASSERT_TRUE(reader.ReadVector(addr, vec_out).ok());
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(vec_out[i], vec_in[i]);
    }
}

TEST_F(DataFileTest, Reader_ReadRaw) {
    std::string path = TestPath("test.dat");

    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, 4).ok());

    float vec_in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec_in, {}, addr).ok());
    ASSERT_TRUE(writer.Finalize().ok());

    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, 4).ok());

    // Read raw bytes for the first float
    uint8_t buf[4];
    ASSERT_TRUE(reader.ReadRaw(0, 4, buf).ok());
    float first;
    std::memcpy(&first, buf, sizeof(float));
    EXPECT_FLOAT_EQ(first, 1.0f);
}

TEST_F(DataFileTest, MixedPayload) {
    std::string path = TestPath("test.dat");
    std::vector<ColumnSchema> schemas = {
        {0, "id", DType::INT64},
        {1, "name", DType::STRING},
        {2, "score", DType::FLOAT32},
    };

    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, 2, schemas).ok());

    float vec[2] = {5.0f, 6.0f};
    std::vector<Datum> payload = {
        Datum::Int64(12345),
        Datum::String("test_record"),
        Datum::Float32(99.5f),
    };

    AddressEntry addr;
    ASSERT_TRUE(writer.WriteRecord(vec, payload, addr).ok());
    ASSERT_TRUE(writer.Finalize().ok());

    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, 2, schemas).ok());

    float vec_out[2];
    std::vector<Datum> payload_out;
    ASSERT_TRUE(reader.ReadRecord(addr, vec_out, payload_out).ok());

    EXPECT_FLOAT_EQ(vec_out[0], 5.0f);
    EXPECT_FLOAT_EQ(vec_out[1], 6.0f);
    ASSERT_EQ(payload_out.size(), 3u);
    EXPECT_EQ(payload_out[0].fixed.i64, 12345);
    EXPECT_EQ(payload_out[1].var_data, "test_record");
    EXPECT_FLOAT_EQ(payload_out[2].fixed.f32, 99.5f);
}

// ============================================================================
// Integration with AddressColumn
// ============================================================================

TEST_F(DataFileTest, Integration_WithAddressColumn) {
    std::string path = TestPath("test.dat");
    const Dim dim = 8;
    const int N = 200;

    // Write records
    DataFileWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, dim).ok());

    std::vector<AddressEntry> addrs;
    std::mt19937 rng(123);
    for (int i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            vec[d] = static_cast<float>(i * dim + d);
        }
        AddressEntry addr;
        ASSERT_TRUE(writer.WriteRecord(vec.data(), {}, addr).ok());
        addrs.push_back(addr);
    }
    ASSERT_TRUE(writer.Finalize().ok());

    // Encode addresses
    auto blocks = AddressColumn::Encode(addrs);

    // Read back using decoded addresses
    DataFileReader reader;
    ASSERT_TRUE(reader.Open(path, dim).ok());

    for (int i = 0; i < N; ++i) {
        auto decoded_addr = AddressColumn::Lookup(blocks, i);
        EXPECT_EQ(decoded_addr.offset, addrs[i].offset);
        EXPECT_EQ(decoded_addr.size, addrs[i].size);

        std::vector<float> vec(dim);
        std::vector<Datum> payload;
        ASSERT_TRUE(reader.ReadRecord(decoded_addr, vec.data(), payload).ok());

        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(vec[d], static_cast<float>(i * dim + d));
        }
    }
}
