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

TEST_F(ClusterStoreTest, Writer_EmptyCluster) {
    ClusterStoreWriter writer;
    RaBitQConfig config{1, 64, 5.75f};
    const Dim dim = 128;

    ASSERT_TRUE(writer.Open(TestPath("test.clu"), 1, dim, config).ok());

    float centroid[128] = {0};
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());

    std::vector<RaBitQCode> codes;
    ASSERT_TRUE(writer.WriteVectors(codes).ok());

    EncodedAddressColumn blocks;
    ASSERT_TRUE(writer.WriteAddressBlocks(blocks).ok());

    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    auto& info = writer.info();
    EXPECT_EQ(info.num_clusters, 1u);
    EXPECT_EQ(info.dim, dim);
    EXPECT_EQ(info.lookup_table[0].num_records, 0u);
    EXPECT_EQ(info.data_file_path, "data.dat");
}

TEST_F(ClusterStoreTest, Writer_MultipleClusters) {
    const Dim dim = 64;
    const uint32_t K = 3;
    const uint32_t N_per_cluster = 10;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    for (uint32_t k = 0; k < K; ++k) {
        std::vector<float> centroid(dim, 0.0f);
        for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

        std::vector<RaBitQCode> codes;
        for (uint32_t i = 0; i < N_per_cluster; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));
        }

        std::vector<AddressEntry> addrs;
        uint64_t off = k * 10000;
        for (uint32_t i = 0; i < N_per_cluster; ++i) {
            uint32_t sz = dim * sizeof(float);
            addrs.push_back({off, sz});
            off += sz;
        }
        auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

        ASSERT_TRUE(writer.BeginCluster(k, N_per_cluster, centroid.data()).ok());
        ASSERT_TRUE(writer.WriteVectors(codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }

    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    auto& info = writer.info();
    EXPECT_EQ(info.num_clusters, K);
    for (uint32_t k = 0; k < K; ++k) {
        EXPECT_EQ(info.lookup_table[k].cluster_id, k);
        EXPECT_EQ(info.lookup_table[k].num_records, N_per_cluster);
        EXPECT_GT(info.lookup_table[k].block_size, 0u);
    }
}

// ============================================================================
// Reader tests
// ============================================================================

TEST_F(ClusterStoreTest, Reader_OpenAndReadLookupTable) {
    const Dim dim = 16;
    const uint32_t K = 2;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    std::vector<float> centroid0(dim);
    std::vector<float> centroid1(dim);
    for (uint32_t d = 0; d < dim; ++d) {
        centroid0[d] = static_cast<float>(d);
        centroid1[d] = static_cast<float>(d + 100);
    }

    // Cluster 0: empty
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid0.data()).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());

    // Cluster 1: empty
    ASSERT_TRUE(writer.BeginCluster(1, 0, centroid1.data()).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());

    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    // Read back
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    EXPECT_EQ(reader.num_clusters(), K);
    EXPECT_EQ(reader.dim(), dim);
    EXPECT_EQ(reader.data_file_path(), "data.dat");

    auto ids = reader.cluster_ids();
    ASSERT_EQ(ids.size(), K);
    EXPECT_EQ(ids[0], 0u);
    EXPECT_EQ(ids[1], 1u);

    EXPECT_EQ(reader.GetNumRecords(0), 0u);
    EXPECT_EQ(reader.GetNumRecords(1), 0u);

    // Verify centroids
    const float* c0 = reader.GetCentroid(0);
    ASSERT_NE(c0, nullptr);
    for (uint32_t d = 0; d < dim; ++d) {
        EXPECT_FLOAT_EQ(c0[d], centroid0[d]);
    }

    const float* c1 = reader.GetCentroid(1);
    ASSERT_NE(c1, nullptr);
    for (uint32_t d = 0; d < dim; ++d) {
        EXPECT_FLOAT_EQ(c1[d], centroid1[d]);
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

    // Address entries
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    // Write single cluster
    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    // Read individual codes
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());

    for (uint32_t i = 0; i < N; ++i) {
        std::vector<uint64_t> out_code;
        ASSERT_TRUE(reader.LoadCode(0, i, out_code).ok());
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
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());

    // Batch load subset
    std::vector<uint32_t> indices = {0, 5, 10, 15, 29};
    std::vector<RaBitQCode> out_codes;
    ASSERT_TRUE(reader.LoadCodes(0, indices, out_codes).ok());

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
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(7, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c7.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(7).ok());

    // Verify address lookup
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = reader.GetAddress(7, i);
        EXPECT_EQ(addr.offset, addrs[i].offset) << "record " << i;
        EXPECT_EQ(addr.size, addrs[i].size) << "record " << i;
    }

    // Batch addresses
    std::vector<uint32_t> indices = {0, 50, 99};
    auto batch_addrs = reader.GetAddresses(7, indices);
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
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());

    float centroid[16] = {0};

    // Can't write vectors without BeginCluster
    EXPECT_FALSE(writer.WriteVectors({}).ok());

    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());

    // Can't write vectors twice
    EXPECT_FALSE(writer.WriteVectors({}).ok());

    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("test.dat").ok());

    // Can't finalize twice
    EXPECT_FALSE(writer.Finalize("test.dat").ok());
}

// ============================================================================
// Multi-cluster roundtrip: write multiple clusters, read back all data
// ============================================================================

TEST_F(ClusterStoreTest, MultiCluster_Roundtrip) {
    const Dim dim = 32;
    const uint32_t K = 4;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(200);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    // Storage for verification
    struct ClusterData {
        std::vector<float> centroid;
        std::vector<RaBitQCode> codes;
        std::vector<AddressEntry> addrs;
        uint32_t cluster_id;
        uint32_t N;
    };
    std::vector<ClusterData> cluster_data(K);

    for (uint32_t k = 0; k < K; ++k) {
        uint32_t N = 10 + k * 5;  // variable sizes
        cluster_data[k].cluster_id = k;
        cluster_data[k].N = N;
        cluster_data[k].centroid.resize(dim);
        for (uint32_t d = 0; d < dim; ++d)
            cluster_data[k].centroid[d] = dist(rng);

        for (uint32_t i = 0; i < N; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            cluster_data[k].codes.push_back(
                encoder.Encode(vec.data(), cluster_data[k].centroid.data()));
        }

        uint64_t off = k * 100000;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t sz = dim * 4u + i * 8;
            cluster_data[k].addrs.push_back({off, sz});
            off += sz;
        }
        auto addr_blocks = AddressColumn::Encode(
            cluster_data[k].addrs, 64, 1);

        ASSERT_TRUE(
            writer.BeginCluster(k, N, cluster_data[k].centroid.data()).ok());
        ASSERT_TRUE(writer.WriteVectors(cluster_data[k].codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }

    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    // --- Read back ---
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    EXPECT_EQ(reader.num_clusters(), K);
    EXPECT_EQ(reader.rabitq_config().bits, 1u);
    EXPECT_EQ(reader.rabitq_config().block_size, 64u);
    EXPECT_FLOAT_EQ(reader.rabitq_config().c_factor, 5.75f);
    EXPECT_EQ(reader.data_file_path(), "data.dat");

    uint64_t expected_total = 0;
    for (uint32_t k = 0; k < K; ++k) {
        expected_total += cluster_data[k].N;
    }
    EXPECT_EQ(reader.total_records(), expected_total);

    for (uint32_t k = 0; k < K; ++k) {
        auto& cd = cluster_data[k];

        EXPECT_EQ(reader.GetNumRecords(k), cd.N);

        // Verify centroid
        const float* c = reader.GetCentroid(k);
        ASSERT_NE(c, nullptr);
        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(c[d], cd.centroid[d]);
        }

        // Load cluster data
        ASSERT_TRUE(reader.EnsureClusterLoaded(k).ok());

        // Verify codes
        for (uint32_t i = 0; i < cd.N; ++i) {
            std::vector<uint64_t> code;
            ASSERT_TRUE(reader.LoadCode(k, i, code).ok());
            ASSERT_EQ(code.size(), cd.codes[i].code.size());
            for (size_t w = 0; w < code.size(); ++w) {
                EXPECT_EQ(code[w], cd.codes[i].code[w]);
            }
        }

        // Verify addresses
        for (uint32_t i = 0; i < cd.N; ++i) {
            auto addr = reader.GetAddress(k, i);
            EXPECT_EQ(addr.offset, cd.addrs[i].offset);
            EXPECT_EQ(addr.size, cd.addrs[i].size);
        }
    }
}

TEST_F(ClusterStoreTest, Reader_EnsureClusterLoaded_Idempotent) {
    const Dim dim = 32;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(333);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    const uint32_t N = 5;
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
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(42, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("d.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    // First load
    ASSERT_TRUE(reader.EnsureClusterLoaded(42).ok());
    // Second load should be no-op / idempotent
    ASSERT_TRUE(reader.EnsureClusterLoaded(42).ok());

    // Data should still be correct
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = reader.GetAddress(42, i);
        EXPECT_EQ(addr.offset, addrs[i].offset);
        EXPECT_EQ(addr.size, addrs[i].size);
    }
}

TEST_F(ClusterStoreTest, Reader_InvalidCluster) {
    const Dim dim = 16;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    float centroid[16] = {0};
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("d.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    // Non-existent cluster
    EXPECT_FALSE(reader.EnsureClusterLoaded(999).ok());
    EXPECT_EQ(reader.GetNumRecords(999), 0u);
    EXPECT_EQ(reader.GetCentroid(999), nullptr);
}

TEST_F(ClusterStoreTest, Reader_InvalidFile) {
    ClusterStoreReader reader;

    // Non-existent file
    EXPECT_FALSE(reader.Open("/nonexistent/path.clu").ok());

    // File with wrong magic
    std::string bad_magic = TestPath("bad.clu");
    {
        std::ofstream f(bad_magic, std::ios::binary);
        uint32_t bad = 0xDEADBEEF;
        f.write(reinterpret_cast<const char*>(&bad), 4);
        uint32_t ver = 2;
        f.write(reinterpret_cast<const char*>(&ver), 4);
    }
    EXPECT_FALSE(reader.Open(bad_magic).ok());
}

TEST_F(ClusterStoreTest, Reader_RejectsOldVersion) {
    std::string path = TestPath("old_version.clu");
    {
        std::ofstream f(path, std::ios::binary);
        uint32_t magic = 0x4C4D4356;
        uint32_t version = 2;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&version), 4);
    }

    ClusterStoreReader reader;
    auto s = reader.Open(path);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsNotSupported());
}

TEST_F(ClusterStoreTest, Reader_RejectsVersion3) {
    // Version 3 used u64 base_offsets; version 4 uses u32.
    // Reader must reject version 3 files.
    std::string path = TestPath("v3.clu");
    {
        std::ofstream f(path, std::ios::binary);
        uint32_t magic = 0x4C4D4356;
        uint32_t version = 3;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&version), 4);
    }

    ClusterStoreReader reader;
    auto s = reader.Open(path);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsNotSupported());
}
