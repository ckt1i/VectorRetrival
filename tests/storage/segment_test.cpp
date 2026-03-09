#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/data_file_writer.h"
#include "vdb/storage/segment.h"

using namespace vdb;
using namespace vdb::storage;
using namespace vdb::rabitq;

namespace fs = std::filesystem;

class SegmentTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_segment_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string TestPath(const std::string& name) {
        return (test_dir_ / name).string();
    }

    // Helper: build a complete cluster (clu + dat files) and return its info.
    ClusterStoreWriter::ClusterInfo BuildCluster(
        uint32_t cluster_id,
        Dim dim,
        uint32_t N,
        const std::vector<ColumnSchema>& payload_schemas = {},
        uint32_t seed = 42) {
        std::mt19937 rng(seed + cluster_id);
        std::normal_distribution<float> dist(0.0f, 1.0f);

        std::string clu_path =
            TestPath("cluster_" + std::to_string(cluster_id) + ".clu");
        std::string dat_path =
            TestPath("cluster_" + std::to_string(cluster_id) + ".dat");

        // Generate centroid
        std::vector<float> centroid(dim);
        for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

        // Rotation + encoder
        RotationMatrix rotation(dim);
        rotation.GenerateRandom(seed);
        RaBitQEncoder encoder(dim, rotation);

        // Generate vectors and write data file
        DataFileWriter dat_writer;
        EXPECT_TRUE(
            dat_writer.Open(dat_path, cluster_id, dim, payload_schemas, 1).ok());

        std::vector<AddressEntry> addrs;
        std::vector<RaBitQCode> codes;

        for (uint32_t i = 0; i < N; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);

            // Encode
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));

            // Build payload
            std::vector<Datum> payload;
            for (const auto& schema : payload_schemas) {
                if (schema.dtype == DType::INT64) {
                    payload.push_back(Datum::Int64(static_cast<int64_t>(i)));
                } else if (schema.dtype == DType::STRING) {
                    payload.push_back(
                        Datum::String("rec_" + std::to_string(i)));
                } else if (schema.dtype == DType::FLOAT32) {
                    payload.push_back(Datum::Float32(static_cast<float>(i)));
                }
            }

            AddressEntry addr;
            EXPECT_TRUE(
                dat_writer.WriteRecord(vec.data(), payload, addr).ok());
            addrs.push_back(addr);
        }
        EXPECT_TRUE(dat_writer.Finalize().ok());

        // Build address blocks
        auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

        // Write cluster store
        RaBitQConfig config{1, 64, 5.75f};
        ClusterStoreWriter clu_writer;
        EXPECT_TRUE(clu_writer.Open(clu_path, cluster_id, dim, config).ok());
        EXPECT_TRUE(clu_writer.WriteCentroid(centroid.data()).ok());
        EXPECT_TRUE(clu_writer.WriteVectors(codes).ok());
        EXPECT_TRUE(clu_writer.WriteAddressBlocks(addr_blocks).ok());
        EXPECT_TRUE(clu_writer.Finalize(dat_path).ok());

        return clu_writer.info();
    }

    fs::path test_dir_;
};

// ============================================================================
// Basic Segment tests
// ============================================================================

TEST_F(SegmentTest, EmptySegment) {
    Segment seg;
    EXPECT_EQ(seg.num_clusters(), 0u);
    EXPECT_EQ(seg.total_records(), 0u);
    EXPECT_TRUE(seg.cluster_ids().empty());
}

TEST_F(SegmentTest, AddCluster_Single) {
    const Dim dim = 64;
    const uint32_t N = 10;

    auto info = BuildCluster(0, dim, N);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info,
                       TestPath("cluster_0.clu"),
                       TestPath("cluster_0.dat"),
                       dim)
            .ok());

    EXPECT_EQ(seg.num_clusters(), 1u);
    EXPECT_EQ(seg.total_records(), N);

    auto ids = seg.cluster_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 0u);
}

TEST_F(SegmentTest, AddCluster_Duplicate) {
    const Dim dim = 32;
    auto info = BuildCluster(5, dim, 5);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info,
                       TestPath("cluster_5.clu"),
                       TestPath("cluster_5.dat"),
                       dim)
            .ok());

    // Adding same cluster_id should fail
    EXPECT_FALSE(
        seg.AddCluster(info,
                       TestPath("cluster_5.clu"),
                       TestPath("cluster_5.dat"),
                       dim)
            .ok());
}

TEST_F(SegmentTest, MultipleCluster_TotalRecords) {
    const Dim dim = 32;

    auto info0 = BuildCluster(0, dim, 20);
    auto info1 = BuildCluster(1, dim, 30);
    auto info2 = BuildCluster(2, dim, 50);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info0,
                       TestPath("cluster_0.clu"),
                       TestPath("cluster_0.dat"),
                       dim)
            .ok());
    ASSERT_TRUE(
        seg.AddCluster(info1,
                       TestPath("cluster_1.clu"),
                       TestPath("cluster_1.dat"),
                       dim)
            .ok());
    ASSERT_TRUE(
        seg.AddCluster(info2,
                       TestPath("cluster_2.clu"),
                       TestPath("cluster_2.dat"),
                       dim)
            .ok());

    EXPECT_EQ(seg.num_clusters(), 3u);
    EXPECT_EQ(seg.total_records(), 100u);

    auto ids = seg.cluster_ids();
    ASSERT_EQ(ids.size(), 3u);
    // Map keeps sorted order
    EXPECT_EQ(ids[0], 0u);
    EXPECT_EQ(ids[1], 1u);
    EXPECT_EQ(ids[2], 2u);
}

// ============================================================================
// GetCluster / GetDataFile
// ============================================================================

TEST_F(SegmentTest, GetCluster_Opens) {
    const Dim dim = 64;
    const uint32_t N = 15;

    auto info = BuildCluster(0, dim, N);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info,
                       TestPath("cluster_0.clu"),
                       TestPath("cluster_0.dat"),
                       dim)
            .ok());

    auto reader = seg.GetCluster(0);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->num_records(), N);
    EXPECT_EQ(reader->dim(), dim);

    // Subsequent call returns cached reader (same pointer)
    auto reader2 = seg.GetCluster(0);
    EXPECT_EQ(reader.get(), reader2.get());
}

TEST_F(SegmentTest, GetCluster_NotFound) {
    Segment seg;
    auto reader = seg.GetCluster(999);
    EXPECT_EQ(reader, nullptr);
}

TEST_F(SegmentTest, GetDataFile_Opens) {
    const Dim dim = 32;
    const uint32_t N = 10;

    auto info = BuildCluster(0, dim, N);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info,
                       TestPath("cluster_0.clu"),
                       TestPath("cluster_0.dat"),
                       dim)
            .ok());

    auto reader = seg.GetDataFile(0);
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->is_open());
    EXPECT_EQ(reader->dim(), dim);

    // Cached
    auto reader2 = seg.GetDataFile(0);
    EXPECT_EQ(reader.get(), reader2.get());
}

TEST_F(SegmentTest, GetDataFile_NotFound) {
    Segment seg;
    auto reader = seg.GetDataFile(42);
    EXPECT_EQ(reader, nullptr);
}

// ============================================================================
// End-to-end: write → Segment → read back vectors
// ============================================================================

TEST_F(SegmentTest, EndToEnd_ReadVectors) {
    const Dim dim = 32;
    const uint32_t N = 25;
    const uint32_t cluster_id = 7;

    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    // Generate centroid and vectors
    std::vector<float> centroid(dim);
    for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::string dat_path = TestPath("c7.dat");
    DataFileWriter dat_writer;
    ASSERT_TRUE(dat_writer.Open(dat_path, cluster_id, dim, {}, 1).ok());

    std::vector<std::vector<float>> original_vecs(N);
    std::vector<AddressEntry> addrs;
    std::vector<RaBitQCode> codes;

    for (uint32_t i = 0; i < N; ++i) {
        original_vecs[i].resize(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            original_vecs[i][d] = dist(rng);
        }

        codes.push_back(
            encoder.Encode(original_vecs[i].data(), centroid.data()));

        AddressEntry addr;
        ASSERT_TRUE(
            dat_writer.WriteRecord(original_vecs[i].data(), {}, addr).ok());
        addrs.push_back(addr);
    }
    ASSERT_TRUE(dat_writer.Finalize().ok());

    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    // Write cluster store
    std::string clu_path = TestPath("c7.clu");
    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter clu_writer;
    ASSERT_TRUE(clu_writer.Open(clu_path, cluster_id, dim, config).ok());
    ASSERT_TRUE(clu_writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(clu_writer.WriteVectors(codes).ok());
    ASSERT_TRUE(clu_writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(clu_writer.Finalize(dat_path).ok());

    auto info = clu_writer.info();

    // Register with segment
    Segment seg;
    ASSERT_TRUE(seg.AddCluster(info, clu_path, dat_path, dim).ok());

    // Read back via segment
    auto clu_reader = seg.GetCluster(cluster_id);
    ASSERT_NE(clu_reader, nullptr);

    auto dat_reader = seg.GetDataFile(cluster_id);
    ASSERT_NE(dat_reader, nullptr);

    // For each record: get address, read vector, verify
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = clu_reader->GetAddress(i);
        EXPECT_EQ(addr.offset, addrs[i].offset) << "record " << i;
        EXPECT_EQ(addr.size, addrs[i].size) << "record " << i;

        std::vector<float> read_vec(dim);
        ASSERT_TRUE(dat_reader->ReadVector(addr, read_vec.data()).ok())
            << "record " << i;

        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(read_vec[d], original_vecs[i][d])
                << "record " << i << " dim " << d;
        }
    }
}

TEST_F(SegmentTest, EndToEnd_WithPayload) {
    const Dim dim = 16;
    const uint32_t N = 10;
    const uint32_t cluster_id = 0;

    std::vector<ColumnSchema> schemas = {
        {0, "id", DType::INT64, false},
        {1, "label", DType::STRING, false},
    };

    std::mt19937 rng(999);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::string dat_path = TestPath("c0.dat");
    DataFileWriter dat_writer;
    ASSERT_TRUE(dat_writer.Open(dat_path, cluster_id, dim, schemas, 1).ok());

    std::vector<std::vector<float>> original_vecs(N);
    std::vector<AddressEntry> addrs;
    std::vector<RaBitQCode> codes;

    for (uint32_t i = 0; i < N; ++i) {
        original_vecs[i].resize(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            original_vecs[i][d] = dist(rng);
        }

        codes.push_back(
            encoder.Encode(original_vecs[i].data(), centroid.data()));

        std::vector<Datum> payload = {
            Datum::Int64(static_cast<int64_t>(i * 100)),
            Datum::String("label_" + std::to_string(i)),
        };

        AddressEntry addr;
        ASSERT_TRUE(
            dat_writer.WriteRecord(original_vecs[i].data(), payload, addr)
                .ok());
        addrs.push_back(addr);
    }
    ASSERT_TRUE(dat_writer.Finalize().ok());

    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    std::string clu_path = TestPath("c0.clu");
    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter clu_writer;
    ASSERT_TRUE(clu_writer.Open(clu_path, cluster_id, dim, config).ok());
    ASSERT_TRUE(clu_writer.WriteCentroid(centroid.data()).ok());
    ASSERT_TRUE(clu_writer.WriteVectors(codes).ok());
    ASSERT_TRUE(clu_writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(clu_writer.Finalize(dat_path).ok());

    auto info = clu_writer.info();

    // Register with segment, passing payload schemas
    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info, clu_path, dat_path, dim, schemas).ok());

    auto dat_reader = seg.GetDataFile(cluster_id);
    ASSERT_NE(dat_reader, nullptr);

    auto clu_reader = seg.GetCluster(cluster_id);
    ASSERT_NE(clu_reader, nullptr);

    // Read back and verify payload
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = clu_reader->GetAddress(i);

        std::vector<float> read_vec(dim);
        std::vector<Datum> read_payload;
        ASSERT_TRUE(
            dat_reader->ReadRecord(addr, read_vec.data(), read_payload).ok());

        // Verify vector
        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(read_vec[d], original_vecs[i][d]);
        }

        // Verify payload
        ASSERT_EQ(read_payload.size(), 2u);
        EXPECT_EQ(read_payload[0].fixed.i64, static_cast<int64_t>(i * 100));
        EXPECT_EQ(read_payload[1].var_data,
                  "label_" + std::to_string(i));
    }
}

TEST_F(SegmentTest, MultiCluster_IndependentAccess) {
    const Dim dim = 32;

    auto info0 = BuildCluster(0, dim, 10);
    auto info1 = BuildCluster(1, dim, 20);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info0,
                       TestPath("cluster_0.clu"),
                       TestPath("cluster_0.dat"),
                       dim)
            .ok());
    ASSERT_TRUE(
        seg.AddCluster(info1,
                       TestPath("cluster_1.clu"),
                       TestPath("cluster_1.dat"),
                       dim)
            .ok());

    // Access cluster 0
    auto r0 = seg.GetCluster(0);
    ASSERT_NE(r0, nullptr);
    EXPECT_EQ(r0->num_records(), 10u);

    // Access cluster 1
    auto r1 = seg.GetCluster(1);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(r1->num_records(), 20u);

    // Both data files accessible
    auto d0 = seg.GetDataFile(0);
    ASSERT_NE(d0, nullptr);
    auto d1 = seg.GetDataFile(1);
    ASSERT_NE(d1, nullptr);

    EXPECT_EQ(seg.total_records(), 30u);
}

TEST_F(SegmentTest, LoadCentroid_ThroughSegment) {
    const Dim dim = 64;
    const uint32_t N = 5;

    auto info = BuildCluster(0, dim, N);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info,
                       TestPath("cluster_0.clu"),
                       TestPath("cluster_0.dat"),
                       dim)
            .ok());

    auto reader = seg.GetCluster(0);
    ASSERT_NE(reader, nullptr);

    std::vector<float> centroid;
    ASSERT_TRUE(reader->LoadCentroid(centroid).ok());
    EXPECT_EQ(centroid.size(), dim);
    // Centroid was generated, just verify it's not all zeros
    bool has_nonzero = false;
    for (uint32_t d = 0; d < dim; ++d) {
        if (centroid[d] != 0.0f) { has_nonzero = true; break; }
    }
    EXPECT_TRUE(has_nonzero);
}

TEST_F(SegmentTest, LoadNorms_ThroughSegment) {
    const Dim dim = 64;
    const uint32_t N = 12;

    auto info = BuildCluster(3, dim, N);

    Segment seg;
    ASSERT_TRUE(
        seg.AddCluster(info,
                       TestPath("cluster_3.clu"),
                       TestPath("cluster_3.dat"),
                       dim)
            .ok());

    auto reader = seg.GetCluster(3);
    ASSERT_NE(reader, nullptr);

    std::vector<float> norms;
    ASSERT_TRUE(reader->LoadAllNorms(norms).ok());
    ASSERT_EQ(norms.size(), N);

    // All norms should be positive (squared distances from centroid)
    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_GT(norms[i], 0.0f) << "norm " << i;
    }
}
