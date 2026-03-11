#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"

using namespace vdb;
using namespace vdb::storage;
using namespace vdb::rabitq;

namespace fs = std::filesystem;

class ClusterStoreTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_cluster_store_test";
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
// Writer tests
// ============================================================================

TEST_F(ClusterStoreTest, Writer_OpenClose) {
    ClusterStoreWriter writer;
    RaBitQConfig config{1, 64, 5.75f};
    ASSERT_TRUE(
        writer.Open(TestPath("test.clu"), 0, 128, config).ok());

    float centroid[128] = {0};
    ASSERT_TRUE(writer.WriteCentroid(centroid).ok());

    std::vector<RaBitQCode> codes;
    ASSERT_TRUE(writer.WriteVectors(codes).ok());

    std::vector<AddressBlock> blocks;
    ASSERT_TRUE(writer.WriteAddressBlocks(blocks).ok());

    ASSERT_TRUE(writer.Finalize("cluster_0.dat").ok());

    auto& info = writer.info();
    EXPECT_EQ(info.cluster_id, 0u);
    EXPECT_EQ(info.dim, 128u);
    EXPECT_EQ(info.num_records, 0u);
}

TEST_F(ClusterStoreTest, Writer_WithVectors) {
    const Dim dim = 64;
    const uint32_t N = 10;
    std::string path = TestPath("test.clu");

    // Create rotation and encoder
    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    // Generate random vectors
    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<std::vector<float>> vectors(N);
    std::vector<RaBitQCode> codes;

    for (uint32_t i = 0; i < N; ++i) {
        vectors[i].resize(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            vectors[i][d] = dist(rng);
        }
        codes.push_back(encoder.Encode(vectors[i].data(), centroid.data()));
    }

    // Create address entries (simulating DataFileWriter output)
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t sz = dim * sizeof(float);
        addrs.push_back({off, sz});
        off += sz;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    // Write cluster store
    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 42, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.Finalize("cluster_42.dat").ok());

    auto& info = writer.info();
    EXPECT_EQ(info.cluster_id, 42u);
    EXPECT_EQ(info.num_records, N);
    EXPECT_EQ(info.dim, dim);
    EXPECT_EQ(info.data_file_path, "cluster_42.dat");
    EXPECT_GT(info.rabitq_data_length, 0u);
}

// ============================================================================
// Reader tests
// ============================================================================

TEST_F(ClusterStoreTest, Reader_LoadCentroid) {
    const Dim dim = 16;
    std::string path = TestPath("test.clu");

    // Write
    std::vector<float> centroid(dim);
    for (uint32_t i = 0; i < dim; ++i) centroid[i] = static_cast<float>(i);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks({}).ok());
    ASSERT_TRUE(writer.Finalize("c0.dat").ok());

    // Read
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path, writer.info()).ok());

    std::vector<float> out;
    ASSERT_TRUE(reader.LoadCentroid(out).ok());
    ASSERT_EQ(out.size(), dim);
    for (uint32_t i = 0; i < dim; ++i) {
        EXPECT_FLOAT_EQ(out[i], centroid[i]);
    }
}

TEST_F(ClusterStoreTest, Reader_LoadCodes) {
    const Dim dim = 128;
    const uint32_t N = 20;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(456);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    // Dummy address blocks
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    // Write
    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    // Read individual codes
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path, writer.info()).ok());
    EXPECT_EQ(reader.num_records(), N);
    EXPECT_EQ(reader.dim(), dim);

    for (uint32_t i = 0; i < N; ++i) {
        std::vector<uint64_t> out_code;
        ASSERT_TRUE(reader.LoadCode(i, out_code).ok());

        ASSERT_EQ(out_code.size(), codes[i].code.size());
        for (size_t w = 0; w < out_code.size(); ++w) {
            EXPECT_EQ(out_code[w], codes[i].code[w])
                << "record " << i << " word " << w;
        }
    }
}

TEST_F(ClusterStoreTest, Reader_LoadCodes_Batch) {
    const Dim dim = 64;
    const uint32_t N = 30;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(789);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path, writer.info()).ok());

    // Batch load subset
    std::vector<uint32_t> indices = {0, 5, 10, 15, 29};
    std::vector<RaBitQCode> out_codes;
    ASSERT_TRUE(reader.LoadCodes(indices, out_codes).ok());

    ASSERT_EQ(out_codes.size(), indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        uint32_t idx = indices[i];
        ASSERT_EQ(out_codes[i].code.size(), codes[idx].code.size());
        for (size_t w = 0; w < out_codes[i].code.size(); ++w) {
            EXPECT_EQ(out_codes[i].code[w], codes[idx].code[w]);
        }
    }
}

TEST_F(ClusterStoreTest, Reader_GetAddress) {
    const Dim dim = 32;
    const uint32_t N = 100;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(222);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    // Variable-size address entries
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t sz = dim * 4u + i * 10;  // variable size
        addrs.push_back({off, sz});
        off += sz;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 7, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.Finalize("c7.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path, writer.info()).ok());

    // Verify address lookup
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = reader.GetAddress(i);
        EXPECT_EQ(addr.offset, addrs[i].offset) << "record " << i;
        EXPECT_EQ(addr.size, addrs[i].size) << "record " << i;
    }

    // Batch addresses
    std::vector<uint32_t> indices = {0, 50, 99};
    auto batch_addrs = reader.GetAddresses(indices);
    ASSERT_EQ(batch_addrs.size(), 3u);
    for (size_t i = 0; i < indices.size(); ++i) {
        EXPECT_EQ(batch_addrs[i].offset, addrs[indices[i]].offset);
        EXPECT_EQ(batch_addrs[i].size, addrs[indices[i]].size);
    }
}

TEST_F(ClusterStoreTest, Writer_OrderConstraints) {
    const Dim dim = 16;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 0, dim, config).ok());

    // Can't write vectors before centroid
    EXPECT_FALSE(writer.WriteVectors({}).ok());

    float centroid[16] = {0};
    ASSERT_TRUE(writer.WriteCentroid(centroid).ok());

    // Can't write centroid twice
    EXPECT_FALSE(writer.WriteCentroid(centroid).ok());

    ASSERT_TRUE(writer.WriteVectors({}).ok());

    // Can't write vectors twice
    EXPECT_FALSE(writer.WriteVectors({}).ok());

    ASSERT_TRUE(writer.WriteAddressBlocks({}).ok());
    ASSERT_TRUE(writer.Finalize("test.dat").ok());

    // Can't finalize twice
    EXPECT_FALSE(writer.Finalize("test.dat").ok());
}

// ============================================================================
// ReadInfo tests — reconstruct ClusterInfo from .clu trailer
// ============================================================================

TEST_F(ClusterStoreTest, ReadInfo_EmptyCluster) {
    const Dim dim = 32;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 7, dim, config).ok());

    std::vector<float> centroid(dim, 1.0f);
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks({}).ok());
    ASSERT_TRUE(writer.Finalize("empty.dat").ok());

    // Read back via ReadInfo
    ClusterStoreWriter::ClusterInfo info;
    ASSERT_TRUE(ClusterStoreReader::ReadInfo(path, &info).ok());

    EXPECT_EQ(info.cluster_id, 7u);
    EXPECT_EQ(info.num_records, 0u);
    EXPECT_EQ(info.dim, dim);
    EXPECT_EQ(info.rabitq_config.bits, 1u);
    EXPECT_EQ(info.rabitq_config.block_size, 64u);
    EXPECT_FLOAT_EQ(info.rabitq_config.c_factor, 5.75f);
    EXPECT_EQ(info.data_file_path, "empty.dat");
    EXPECT_TRUE(info.address_blocks.empty());
}

TEST_F(ClusterStoreTest, ReadInfo_WithVectors) {
    const Dim dim = 64;
    const uint32_t N = 10;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(100);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 42, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.Finalize("c42.dat").ok());

    const auto& original = writer.info();

    // ReadInfo should reconstruct equivalent ClusterInfo
    ClusterStoreWriter::ClusterInfo info;
    ASSERT_TRUE(ClusterStoreReader::ReadInfo(path, &info).ok());

    EXPECT_EQ(info.cluster_id, original.cluster_id);
    EXPECT_EQ(info.num_records, original.num_records);
    EXPECT_EQ(info.dim, original.dim);
    EXPECT_EQ(info.rabitq_config.bits, original.rabitq_config.bits);
    EXPECT_EQ(info.rabitq_config.block_size,
              original.rabitq_config.block_size);
    EXPECT_FLOAT_EQ(info.rabitq_config.c_factor,
                    original.rabitq_config.c_factor);
    EXPECT_EQ(info.data_file_path, original.data_file_path);
    EXPECT_EQ(info.centroid_offset, original.centroid_offset);
    EXPECT_EQ(info.centroid_length, original.centroid_length);
    EXPECT_EQ(info.rabitq_data_offset, original.rabitq_data_offset);
    EXPECT_EQ(info.rabitq_data_length, original.rabitq_data_length);
    ASSERT_EQ(info.address_blocks.size(), original.address_blocks.size());
    EXPECT_EQ(info.address_blocks_offset, original.address_blocks_offset);
    for (size_t i = 0; i < info.address_blocks.size(); ++i) {
        EXPECT_EQ(info.address_blocks[i].base_offset,
                  original.address_blocks[i].base_offset);
        EXPECT_EQ(info.address_blocks[i].bit_width,
                  original.address_blocks[i].bit_width);
        EXPECT_EQ(info.address_blocks[i].record_count,
                  original.address_blocks[i].record_count);
        EXPECT_EQ(info.address_blocks[i].page_size,
                  original.address_blocks[i].page_size);
        // ReadInfo only restores packed.size() (capacity for LoadAddressBlocks);
        // packed content is filled by LoadAddressBlocks when Open() is called.
        EXPECT_EQ(info.address_blocks[i].packed.size(),
                  original.address_blocks[i].packed.size());
    }
}

TEST_F(ClusterStoreTest, ReadInfo_ThenOpenReader) {
    const Dim dim = 32;
    const uint32_t N = 20;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(200);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim);
    for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    // Variable-size address entries
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t sz = dim * 4u + i * 8;
        addrs.push_back({off, sz});
        off += sz;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 3, dim, config).ok());
    ASSERT_TRUE(writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.Finalize("c3.dat").ok());

    // Use ReadInfo to get ClusterInfo, then open a reader with it
    ClusterStoreWriter::ClusterInfo info;
    ASSERT_TRUE(ClusterStoreReader::ReadInfo(path, &info).ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path, info).ok());
    EXPECT_EQ(reader.num_records(), N);
    EXPECT_EQ(reader.dim(), dim);
    EXPECT_EQ(reader.cluster_id(), 3u);

    // Verify centroid
    std::vector<float> read_centroid;
    ASSERT_TRUE(reader.LoadCentroid(read_centroid).ok());
    for (uint32_t d = 0; d < dim; ++d) {
        EXPECT_FLOAT_EQ(read_centroid[d], centroid[d]);
    }

    // Verify codes
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<uint64_t> code;
        ASSERT_TRUE(reader.LoadCode(i, code).ok());
        ASSERT_EQ(code.size(), codes[i].code.size());
        for (size_t w = 0; w < code.size(); ++w) {
            EXPECT_EQ(code[w], codes[i].code[w]);
        }
    }

    // Verify addresses
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = reader.GetAddress(i);
        EXPECT_EQ(addr.offset, addrs[i].offset) << "record " << i;
        EXPECT_EQ(addr.size, addrs[i].size) << "record " << i;
    }
}

TEST_F(ClusterStoreTest, ReadInfo_InvalidFile) {
    // Non-existent file
    ClusterStoreWriter::ClusterInfo info;
    EXPECT_FALSE(
        ClusterStoreReader::ReadInfo("/nonexistent/path.clu", &info).ok());

    // File too small
    std::string tiny_path = TestPath("tiny.clu");
    {
        std::ofstream f(tiny_path, std::ios::binary);
        f << "abc";
    }
    EXPECT_FALSE(ClusterStoreReader::ReadInfo(tiny_path, &info).ok());

    // File with wrong magic
    std::string bad_magic = TestPath("bad_magic.clu");
    {
        std::ofstream f(bad_magic, std::ios::binary);
        uint32_t zero = 0;
        uint32_t bad = 0xDEADBEEF;
        f.write(reinterpret_cast<const char*>(&zero), 4);
        f.write(reinterpret_cast<const char*>(&bad), 4);
    }
    EXPECT_FALSE(ClusterStoreReader::ReadInfo(bad_magic, &info).ok());
}