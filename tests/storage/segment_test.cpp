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

    /// Helper: build a unified cluster.clu + data.dat in test_dir_ with
    /// K clusters, each with N records of dimension `dim`.
    /// Returns the original vectors for verification.
    struct BuiltSegment {
        std::vector<std::vector<std::vector<float>>> vecs;  // [k][i][d]
        std::vector<std::vector<AddressEntry>> addrs;       // [k][i]
        std::vector<std::vector<float>> centroids;          // [k]
        std::vector<std::vector<RaBitQCode>> codes;         // [k]
    };

    BuiltSegment BuildSegment(
        uint32_t K, uint32_t N_per_cluster, Dim dim,
        const std::vector<ColumnSchema>& payload_schemas = {},
        uint32_t seed = 42) {
        BuiltSegment result;
        result.vecs.resize(K);
        result.addrs.resize(K);
        result.centroids.resize(K);
        result.codes.resize(K);

        std::mt19937 rng(seed);
        std::normal_distribution<float> dist(0.0f, 1.0f);

        RotationMatrix rotation(dim);
        rotation.GenerateRandom(seed);
        RaBitQEncoder encoder(dim, rotation);

        // --- Write data.dat ---
        DataFileWriter dat_writer;
        std::string dat_path = (test_dir_ / "data.dat").string();
        EXPECT_TRUE(
            dat_writer.Open(dat_path, 0, dim, payload_schemas, 1).ok());

        for (uint32_t k = 0; k < K; ++k) {
            result.centroids[k].resize(dim);
            for (uint32_t d = 0; d < dim; ++d)
                result.centroids[k][d] = dist(rng);

            result.vecs[k].resize(N_per_cluster);
            result.addrs[k].reserve(N_per_cluster);

            for (uint32_t i = 0; i < N_per_cluster; ++i) {
                result.vecs[k][i].resize(dim);
                for (uint32_t d = 0; d < dim; ++d)
                    result.vecs[k][i][d] = dist(rng);

                result.codes[k].push_back(
                    encoder.Encode(result.vecs[k][i].data(),
                                   result.centroids[k].data()));

                // Build payload
                std::vector<Datum> payload;
                for (const auto& schema : payload_schemas) {
                    if (schema.dtype == DType::INT64) {
                        payload.push_back(
                            Datum::Int64(static_cast<int64_t>(k * 1000 + i)));
                    } else if (schema.dtype == DType::STRING) {
                        payload.push_back(
                            Datum::String("r_" + std::to_string(k) + "_" +
                                          std::to_string(i)));
                    } else if (schema.dtype == DType::FLOAT32) {
                        payload.push_back(
                            Datum::Float32(static_cast<float>(i)));
                    }
                }

                AddressEntry addr;
                EXPECT_TRUE(
                    dat_writer.WriteRecord(result.vecs[k][i].data(),
                                           payload, addr)
                        .ok());
                result.addrs[k].push_back(addr);
            }
        }
        EXPECT_TRUE(dat_writer.Finalize().ok());

        // --- Write cluster.clu ---
        RaBitQConfig config{1, 64, 5.75f};
        ClusterStoreWriter clu_writer;
        std::string clu_path = (test_dir_ / "cluster.clu").string();
        EXPECT_TRUE(clu_writer.Open(clu_path, K, dim, config).ok());

        for (uint32_t k = 0; k < K; ++k) {
            auto addr_blocks = AddressColumn::Encode(
                result.addrs[k], 64, 1);

            EXPECT_TRUE(clu_writer.BeginCluster(
                k, N_per_cluster, result.centroids[k].data()).ok());
            EXPECT_TRUE(clu_writer.WriteVectors(result.codes[k]).ok());
            EXPECT_TRUE(clu_writer.WriteAddressBlocks(addr_blocks).ok());
            EXPECT_TRUE(clu_writer.EndCluster().ok());
        }

        EXPECT_TRUE(clu_writer.Finalize("data.dat").ok());
        return result;
    }

    fs::path test_dir_;
};

// ============================================================================
// Basic Segment tests
// ============================================================================

TEST_F(SegmentTest, Open_EmptyDir_Fails) {
    Segment seg;
    EXPECT_FALSE(seg.Open(test_dir_.string()).ok());
}

TEST_F(SegmentTest, Open_SingleCluster) {
    const Dim dim = 64;
    const uint32_t K = 1;
    const uint32_t N = 10;

    BuildSegment(K, N, dim);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string()).ok());
    EXPECT_TRUE(seg.is_open());
    EXPECT_EQ(seg.num_clusters(), K);
    EXPECT_EQ(seg.total_records(), N);
    EXPECT_EQ(seg.dim(), dim);

    auto ids = seg.cluster_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 0u);
}

TEST_F(SegmentTest, Open_MultipleClusters) {
    const Dim dim = 32;
    const uint32_t K = 3;
    const uint32_t N = 15;

    BuildSegment(K, N, dim);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string()).ok());
    EXPECT_EQ(seg.num_clusters(), K);
    EXPECT_EQ(seg.total_records(), K * N);

    auto ids = seg.cluster_ids();
    ASSERT_EQ(ids.size(), K);
    for (uint32_t k = 0; k < K; ++k) {
        EXPECT_EQ(ids[k], k);
        EXPECT_EQ(seg.GetNumRecords(k), N);
    }
}

// ============================================================================
// Centroid access
// ============================================================================

TEST_F(SegmentTest, GetCentroid) {
    const Dim dim = 64;
    const uint32_t K = 2;
    const uint32_t N = 5;

    auto built = BuildSegment(K, N, dim);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string()).ok());

    for (uint32_t k = 0; k < K; ++k) {
        const float* c = seg.GetCentroid(k);
        ASSERT_NE(c, nullptr);
        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(c[d], built.centroids[k][d]);
        }
    }

    // Non-existent cluster
    EXPECT_EQ(seg.GetCentroid(999), nullptr);
}

// ============================================================================
// End-to-end: write → Segment → read back vectors
// ============================================================================

TEST_F(SegmentTest, EndToEnd_ReadVectors) {
    const Dim dim = 32;
    const uint32_t K = 2;
    const uint32_t N = 20;

    auto built = BuildSegment(K, N, dim);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string()).ok());

    for (uint32_t k = 0; k < K; ++k) {
        ASSERT_TRUE(seg.EnsureClusterLoaded(k).ok());

        for (uint32_t i = 0; i < N; ++i) {
            auto addr = seg.GetAddress(k, i);
            EXPECT_EQ(addr.offset, built.addrs[k][i].offset);
            EXPECT_EQ(addr.size, built.addrs[k][i].size);

            std::vector<float> read_vec(dim);
            ASSERT_TRUE(seg.ReadVector(addr, read_vec.data()).ok());

            for (uint32_t d = 0; d < dim; ++d) {
                EXPECT_FLOAT_EQ(read_vec[d], built.vecs[k][i][d])
                    << "k=" << k << " i=" << i << " d=" << d;
            }
        }
    }
}

TEST_F(SegmentTest, EndToEnd_LoadCodes) {
    const Dim dim = 64;
    const uint32_t K = 2;
    const uint32_t N = 10;

    auto built = BuildSegment(K, N, dim);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string()).ok());

    for (uint32_t k = 0; k < K; ++k) {
        ASSERT_TRUE(seg.EnsureClusterLoaded(k).ok());

        for (uint32_t i = 0; i < N; ++i) {
            std::vector<uint64_t> code;
            ASSERT_TRUE(seg.LoadCode(k, i, code).ok());
            ASSERT_EQ(code.size(), built.codes[k][i].code.size());
            for (size_t w = 0; w < code.size(); ++w) {
                EXPECT_EQ(code[w], built.codes[k][i].code[w]);
            }
        }
    }
}

TEST_F(SegmentTest, EndToEnd_WithPayload) {
    const Dim dim = 16;
    const uint32_t K = 1;
    const uint32_t N = 10;

    std::vector<ColumnSchema> schemas = {
        {0, "id", DType::INT64, false},
        {1, "label", DType::STRING, false},
    };

    auto built = BuildSegment(K, N, dim, schemas);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string(), schemas).ok());

    ASSERT_TRUE(seg.EnsureClusterLoaded(0).ok());

    for (uint32_t i = 0; i < N; ++i) {
        auto addr = seg.GetAddress(0, i);

        std::vector<float> read_vec(dim);
        std::vector<Datum> read_payload;
        ASSERT_TRUE(
            seg.ReadRecord(addr, read_vec.data(), read_payload).ok());

        // Verify vector
        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(read_vec[d], built.vecs[0][i][d]);
        }

        // Verify payload
        ASSERT_EQ(read_payload.size(), 2u);
        EXPECT_EQ(read_payload[0].fixed.i64,
                  static_cast<int64_t>(0 * 1000 + i));
        EXPECT_EQ(read_payload[1].var_data,
                  "r_0_" + std::to_string(i));
    }
}

TEST_F(SegmentTest, RaBitQConfig_Accessible) {
    const Dim dim = 32;
    BuildSegment(1, 5, dim);

    Segment seg;
    ASSERT_TRUE(seg.Open(test_dir_.string()).ok());

    auto& cfg = seg.rabitq_config();
    EXPECT_EQ(cfg.bits, 1u);
    EXPECT_EQ(cfg.block_size, 64u);
    EXPECT_FLOAT_EQ(cfg.c_factor, 5.75f);
}

