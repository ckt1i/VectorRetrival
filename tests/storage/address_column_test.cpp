#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/storage/address_column.h"

using namespace vdb;
using namespace vdb::storage;

// ============================================================================
// Local helpers replacing the removed Lookup / BatchLookup static methods.
// ClusterStoreReader now eagerly decodes all addresses at Open() time, so
// Lookup/BatchLookup are no longer part of the public API; tests use
// DecodeBlock directly.
// ============================================================================

static AddressEntry TestLookup(
    const EncodedAddressColumn& column,
    uint32_t idx,
    uint32_t gran = 0) {
    if (gran == 0) {
        gran = column.layout.block_granularity;
    }
    if (gran == 0) {
        return AddressEntry{0, 0};
    }
    uint32_t block_idx = idx / gran;
    if (block_idx >= static_cast<uint32_t>(column.blocks.size())) {
        return AddressEntry{0, 0};
    }
    uint32_t local_idx = idx % gran;
    std::vector<AddressEntry> entries;
    AddressColumn::DecodeSingleBlock(
        column.layout, column.blocks[block_idx],
        AddressColumn::BlockRecordCount(column.layout, column.total_records, block_idx),
        entries);
    if (local_idx >= static_cast<uint32_t>(entries.size())) {
        return AddressEntry{0, 0};
    }
    return entries[local_idx];
}

static std::vector<AddressEntry> TestBatchLookup(
    const EncodedAddressColumn& column,
    const std::vector<uint32_t>& indices,
    uint32_t gran = 0) {
    std::vector<AddressEntry> results;
    results.reserve(indices.size());
    for (auto idx : indices) {
        results.push_back(TestLookup(column, idx, gran));
    }
    return results;
}

// ============================================================================
// Encode / Decode round-trip
// ============================================================================

TEST(AddressColumnTest, EmptyInput) {
    std::vector<AddressEntry> entries;
    auto column = AddressColumn::Encode(entries);
    EXPECT_TRUE(column.blocks.empty());
    EXPECT_EQ(AddressColumn::TotalRecords(column), 0u);
}

TEST(AddressColumnTest, SingleRecord) {
    std::vector<AddressEntry> entries = {{1000, 256}};
    auto column = AddressColumn::Encode(entries, 64, 1);

    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(column.blocks[0].base_offset, 1000u);
    EXPECT_EQ(AddressColumn::BlockRecordCount(column.layout, column.total_records, 0), 1u);
    EXPECT_EQ(AddressColumn::TotalRecords(column), 1u);

    // Decode
    auto e = TestLookup(column, 0);
    EXPECT_EQ(e.offset, 1000u);
    EXPECT_EQ(e.size, 256u);
}

TEST(AddressColumnTest, ExactlyOneBlock) {
    // 64 records → exactly one block
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        uint32_t sz = 100 + i;
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(AddressColumn::BlockRecordCount(column.layout, column.total_records, 0), 64u);

    // Verify round-trip for every entry
    for (uint32_t i = 0; i < 64; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, TwoBlocks) {
    // 100 records → 2 blocks (64 + 36)
    std::vector<AddressEntry> entries;
    uint64_t off = 5000;
    for (int i = 0; i < 100; ++i) {
        uint32_t sz = 200;
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 2u);
    EXPECT_EQ(AddressColumn::BlockRecordCount(column.layout, column.total_records, 0), 64u);
    EXPECT_EQ(AddressColumn::BlockRecordCount(column.layout, column.total_records, 1), 36u);
    EXPECT_EQ(AddressColumn::TotalRecords(column), 100u);

    // Verify all entries
    for (uint32_t i = 0; i < 100; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, UniformSizes) {
    // All records same size → bit_width should be minimal
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        entries.push_back({off, 128});
        off += 128;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(column.layout.bit_width, 8u);  // 128 needs 8 bits

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset);
        EXPECT_EQ(e.size, 128u);
    }
}

TEST(AddressColumnTest, SmallSizes_LowBitWidth) {
    // sizes all ≤ 3 → bit_width = 2
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        uint32_t sz = (i % 4);  // 0, 1, 2, 3
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(column.layout.bit_width, 2u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, LargeSizes) {
    // sizes up to 100000 → needs 17 bits
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        uint32_t sz = 50000 + i * 800;  // max ~= 100000
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 3u);
    EXPECT_GE(column.layout.bit_width, 17u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

// ============================================================================
// DecodeBlock
// ============================================================================

TEST(AddressColumnTest, DecodeBlock_Full) {
    std::vector<AddressEntry> entries;
    uint64_t off = 1000;
    for (int i = 0; i < 64; ++i) {
        entries.push_back({off, 512u});
        off += 512;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_GE(column.blocks.size(), 1u);
    const uint32_t full_block_count = column.layout.block_granularity;

    std::vector<AddressEntry> decoded;
    AddressColumn::DecodeSingleBlock(
        column.layout, column.blocks[0], full_block_count, decoded);
    ASSERT_EQ(decoded.size(), full_block_count);

    for (uint32_t i = 0; i < full_block_count; ++i) {
        EXPECT_EQ(decoded[i].offset, entries[i].offset);
        EXPECT_EQ(decoded[i].size, entries[i].size);
    }
}

TEST(AddressColumnTest, DecodeBlock_Partial) {
    // Less than 64 records in last block
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 10; ++i) {
        entries.push_back({off, 100u});
        off += 100;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(AddressColumn::BlockRecordCount(column.layout, column.total_records, 0), 10u);

    std::vector<AddressEntry> decoded;
    AddressColumn::DecodeSingleBlock(column.layout, column.blocks[0], 10, decoded);
    ASSERT_EQ(decoded.size(), 10u);
}

// ============================================================================
// BatchLookup
// ============================================================================

TEST(AddressColumnTest, BatchLookup_SameBlock) {
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        entries.push_back({off, 200u + static_cast<uint32_t>(i)});
        off += 200 + i;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);

    std::vector<uint32_t> indices = {0, 10, 32, 63};
    auto results = TestBatchLookup(column, indices);

    ASSERT_EQ(results.size(), 4u);
    for (size_t i = 0; i < indices.size(); ++i) {
        EXPECT_EQ(results[i].offset, entries[indices[i]].offset);
        EXPECT_EQ(results[i].size, entries[indices[i]].size);
    }
}

TEST(AddressColumnTest, BatchLookup_CrossBlock) {
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 200; ++i) {
        entries.push_back({off, 300u});
        off += 300;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    EXPECT_EQ(column.blocks.size(), 4u);  // 200/64 = 3.125 → 4 blocks

    std::vector<uint32_t> indices = {0, 63, 64, 127, 128, 199};
    auto results = TestBatchLookup(column, indices);

    ASSERT_EQ(results.size(), 6u);
    for (size_t i = 0; i < indices.size(); ++i) {
        EXPECT_EQ(results[i].offset, entries[indices[i]].offset)
            << "index " << indices[i];
        EXPECT_EQ(results[i].size, entries[indices[i]].size)
            << "index " << indices[i];
    }
}

// ============================================================================
// Out-of-range
// ============================================================================

TEST(AddressColumnTest, Lookup_OutOfRange) {
    std::vector<AddressEntry> entries = {{0, 100}, {100, 100}};
    auto column = AddressColumn::Encode(entries, 64, 1);

    auto e = TestLookup(column, 999);
    EXPECT_EQ(e.offset, 0u);
    EXPECT_EQ(e.size, 0u);
}

// ============================================================================
// Random stress test
// ============================================================================

TEST(AddressColumnTest, RandomStress) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> size_dist(1, 4096);

    const int N = 500;
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < N; ++i) {
        uint32_t sz = size_dist(rng);
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    EXPECT_EQ(AddressColumn::TotalRecords(column),
              static_cast<uint32_t>(N));

    // Verify every entry
    for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

// ============================================================================
// Custom granularity
// ============================================================================

TEST(AddressColumnTest, FixedPackedSize_32) {
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 100; ++i) {
        entries.push_back({off, 256u});
        off += 256;
    }

    auto column = AddressColumn::Encode(entries, 32, 1);
    EXPECT_EQ(column.blocks.size(), 4u);  // 100/32 = 3.125 → 4
    EXPECT_EQ(column.layout.fixed_packed_size, 32u);

    for (uint32_t i = 0; i < 100; ++i) {
        auto e = TestLookup(column, i, column.layout.block_granularity);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, LastPackedSize_TracksTailBlock) {
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 100; ++i) {
        entries.push_back({off, 200u});
        off += 200;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 2u);
    EXPECT_EQ(column.layout.fixed_packed_size, 64u);
    EXPECT_LT(column.layout.last_packed_size, column.layout.fixed_packed_size);
    EXPECT_EQ(column.blocks[0].packed.size(), column.layout.fixed_packed_size);
    EXPECT_EQ(column.blocks[1].packed.size(), column.layout.last_packed_size);
}

TEST(AddressColumnTest, ZeroSizes) {
    // Edge case: all sizes = 0
    std::vector<AddressEntry> entries;
    for (int i = 0; i < 64; ++i) {
        entries.push_back({0, 0});
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.blocks.size(), 1u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, 0u);
        EXPECT_EQ(e.size, 0u);
    }
}

// ============================================================================
// Page-aligned tests (page_size = 4096)
// ============================================================================

TEST(AddressColumnTest, PageAligned_SingleRecord) {
    // A single record occupying exactly one 4KB page
    std::vector<AddressEntry> entries = {{0, 4096}};
    auto column = AddressColumn::Encode(entries, 64, 4096);

    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(column.layout.page_size, 4096u);
    EXPECT_EQ(column.blocks[0].base_offset, 0u);  // page index 0

    auto e = TestLookup(column, 0);
    EXPECT_EQ(e.offset, 0u);
    EXPECT_EQ(e.size, 4096u);
}

TEST(AddressColumnTest, PageAligned_MultipleRecords) {
    // 10 records, each 2 pages (8192 bytes)
    const uint32_t page_size = 4096;
    const uint32_t pages_per_record = 2;
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 10; ++i) {
        uint32_t sz = pages_per_record * page_size;
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_EQ(column.blocks.size(), 1u);
    EXPECT_EQ(column.layout.page_size, page_size);
    // base_offset in page units: 0
    EXPECT_EQ(column.blocks[0].base_offset, 0u);
    // bit_width: all sizes are 2 pages, needs 2 bits
    EXPECT_EQ(column.layout.bit_width, 2u);

    for (uint32_t i = 0; i < 10; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, PageAligned_VariableSizes) {
    // Records of varying page counts (1-5 pages each)
    const uint32_t page_size = 4096;
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        uint32_t pages = 1 + (i % 5);  // 1 to 5 pages
        uint32_t sz = pages * page_size;
        entries.push_back({off, sz});
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_EQ(column.blocks.size(), 1u);
    // max pages = 5, needs 3 bits
    EXPECT_EQ(column.layout.bit_width, 3u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = TestLookup(column, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, PageAligned_BitWidthReduction) {
    // Without page alignment: size=8192 needs 14 bits
    // With page_size=4096:    size=2 pages needs 2 bits
    const uint32_t page_size = 4096;
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 64; ++i) {
        entries.push_back({off, 8192u});
        off += 8192;
    }

    // page_size=1 (byte-level): needs 14 bits
    auto column_byte = AddressColumn::Encode(entries, 64, 1);
    EXPECT_EQ(column_byte.layout.bit_width, 14u);

    // page_size=4096: needs 2 bits (size=2 in page units)
    auto column_page = AddressColumn::Encode(entries, 64, page_size);
    EXPECT_EQ(column_page.layout.bit_width, 2u);

    // Both decode to the same byte addresses
    for (uint32_t i = 0; i < 64; ++i) {
        auto e1 = TestLookup(column_byte, i);
        auto e2 = TestLookup(column_page, i);
        EXPECT_EQ(e1.offset, e2.offset);
        EXPECT_EQ(e1.size, e2.size);
    }
}

// ============================================================================
// Multi-stream decode vs serial decode consistency
// ============================================================================

/// Helper: decode column using DecodeSingleBlock per block (serial baseline)
static std::vector<AddressEntry> SerialDecode(const EncodedAddressColumn& column) {
    std::vector<AddressEntry> all;
    for (uint32_t b = 0; b < static_cast<uint32_t>(column.blocks.size()); ++b) {
        uint32_t count = AddressColumn::BlockRecordCount(
            column.layout, column.total_records, b);
        std::vector<AddressEntry> block_entries;
        AddressColumn::DecodeSingleBlock(
            column.layout, column.blocks[b], count, block_entries);
        all.insert(all.end(), block_entries.begin(), block_entries.end());
    }
    return all;
}

/// Helper: build entries with N records, uniform size, starting at given offset
static std::vector<AddressEntry> MakeUniformEntries(
    uint32_t N, uint32_t record_size, uint64_t start_offset = 0) {
    std::vector<AddressEntry> entries(N);
    uint64_t off = start_offset;
    for (uint32_t i = 0; i < N; ++i) {
        entries[i] = {off, record_size};
        off += record_size;
    }
    return entries;
}

// Parameterized test: multi-stream decode == serial decode for various block counts
class MultiStreamConsistencyTest
    : public ::testing::TestWithParam<uint32_t /*num_blocks*/> {};

TEST_P(MultiStreamConsistencyTest, MatchesSerialDecode) {
    const uint32_t target_blocks = GetParam();
    // Use page_size=1 so sizes map 1:1 to page units
    const uint32_t record_size = 200;
    // Encode with fixed_packed_size=64 so granularity is known
    // First encode a small set to discover granularity, then build the real one
    auto probe = AddressColumn::Encode({{0, record_size}}, 64, 1);
    const uint32_t G = probe.layout.block_granularity;
    ASSERT_GT(G, 0u);

    // Build enough records to produce exactly target_blocks blocks
    // (target_blocks - 1) full blocks + 1 partial block with half records
    const uint32_t N = (target_blocks > 1)
        ? (target_blocks - 1) * G + G / 2
        : G / 2;
    auto entries = MakeUniformEntries(N, record_size);
    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(column.layout.num_address_blocks, target_blocks);

    // Decode via multi-stream pipeline (DecodeBatchBlocks)
    std::vector<AddressEntry> multi_result;
    auto status = AddressColumn::DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, multi_result);
    ASSERT_TRUE(status.ok()) << status.ToString();

    // Decode via serial single-block baseline
    auto serial_result = SerialDecode(column);

    ASSERT_EQ(multi_result.size(), serial_result.size());
    for (size_t i = 0; i < multi_result.size(); ++i) {
        EXPECT_EQ(multi_result[i].offset, serial_result[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, serial_result[i].size)
            << "record " << i;
    }

    // Also verify against original entries
    ASSERT_EQ(multi_result.size(), N);
    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_EQ(multi_result[i].offset, entries[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, entries[i].size)
            << "record " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    BatchBoundary, MultiStreamConsistencyTest,
    ::testing::Values(1, 2, 3, 7, 8, 9, 16, 17));

// Variable-size records with multi-stream decode
TEST(MultiStreamDecodeTest, VariableSizes_Consistency) {
    std::mt19937 rng(123);
    std::uniform_int_distribution<uint32_t> size_dist(1, 1024);

    const uint32_t N = 700;  // produces many blocks
    std::vector<AddressEntry> entries(N);
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t sz = size_dist(rng);
        entries[i] = {off, sz};
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, 1);
    ASSERT_GT(column.layout.num_address_blocks, 8u);

    std::vector<AddressEntry> multi_result;
    auto status = AddressColumn::DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, multi_result);
    ASSERT_TRUE(status.ok());

    auto serial_result = SerialDecode(column);
    ASSERT_EQ(multi_result.size(), serial_result.size());
    for (size_t i = 0; i < multi_result.size(); ++i) {
        EXPECT_EQ(multi_result[i].offset, serial_result[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, serial_result[i].size)
            << "record " << i;
    }
}

// Page-aligned multi-stream decode
TEST(MultiStreamDecodeTest, PageAligned_Consistency) {
    const uint32_t page_size = 4096;
    const uint32_t N = 600;
    std::vector<AddressEntry> entries(N);
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t pages = 1 + (i % 4);
        uint32_t sz = pages * page_size;
        entries[i] = {off, sz};
        off += sz;
    }

    auto column = AddressColumn::Encode(entries, 64, page_size);

    std::vector<AddressEntry> multi_result;
    auto status = AddressColumn::DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, multi_result);
    ASSERT_TRUE(status.ok());

    auto serial_result = SerialDecode(column);
    ASSERT_EQ(multi_result.size(), serial_result.size());
    for (size_t i = 0; i < multi_result.size(); ++i) {
        EXPECT_EQ(multi_result[i].offset, serial_result[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, serial_result[i].size)
            << "record " << i;
    }
}

// ============================================================================
// base_offset near UINT32_MAX boundary tests
// ============================================================================

TEST(MultiStreamDecodeTest, BaseOffset_NearUint32Max) {
    // Place records so base_offset (page index) is near UINT32_MAX
    const uint32_t page_size = 4096;
    // Start offset such that page index ≈ UINT32_MAX - 100
    const uint64_t start_page = static_cast<uint64_t>(UINT32_MAX) - 100;
    const uint64_t start_offset = start_page * page_size;
    const uint32_t record_size = page_size;  // 1 page each
    const uint32_t N = 50;

    std::vector<AddressEntry> entries(N);
    uint64_t off = start_offset;
    for (uint32_t i = 0; i < N; ++i) {
        entries[i] = {off, record_size};
        off += record_size;
    }

    auto column = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_GE(column.blocks.size(), 1u);
    // Verify base_offset is near UINT32_MAX
    EXPECT_GE(column.blocks[0].base_offset, UINT32_MAX - 100);

    std::vector<AddressEntry> multi_result;
    auto status = AddressColumn::DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, multi_result);
    ASSERT_TRUE(status.ok());

    ASSERT_EQ(multi_result.size(), N);
    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_EQ(multi_result[i].offset, entries[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, entries[i].size)
            << "record " << i;
    }
}

TEST(MultiStreamDecodeTest, BaseOffset_ExactlyUint32Max) {
    // base_offset == UINT32_MAX (extreme boundary)
    const uint32_t page_size = 1;
    const uint64_t start_offset = static_cast<uint64_t>(UINT32_MAX);
    const uint32_t record_size = 1;
    const uint32_t N = 10;

    std::vector<AddressEntry> entries(N);
    uint64_t off = start_offset;
    for (uint32_t i = 0; i < N; ++i) {
        entries[i] = {off, record_size};
        off += record_size;
    }

    auto column = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_GE(column.blocks.size(), 1u);
    EXPECT_EQ(column.blocks[0].base_offset, UINT32_MAX);

    std::vector<AddressEntry> multi_result;
    auto status = AddressColumn::DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, multi_result);
    ASSERT_TRUE(status.ok());

    ASSERT_EQ(multi_result.size(), N);
    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_EQ(multi_result[i].offset, entries[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, entries[i].size)
            << "record " << i;
    }
}

TEST(MultiStreamDecodeTest, MultipleBlocks_HighBaseOffset) {
    // Multiple blocks with high base_offsets, tests multi-stream pipeline
    const uint32_t page_size = 4096;
    const uint64_t start_page = static_cast<uint64_t>(UINT32_MAX) - 10000;
    const uint64_t start_offset = start_page * page_size;
    const uint32_t record_size = 2 * page_size;  // 2 pages each
    const uint32_t N = 3000;  // enough for many blocks (>8 with granularity ~256)

    std::vector<AddressEntry> entries(N);
    uint64_t off = start_offset;
    for (uint32_t i = 0; i < N; ++i) {
        entries[i] = {off, record_size};
        off += record_size;
    }

    auto column = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_GT(column.layout.num_address_blocks, 8u);

    std::vector<AddressEntry> multi_result;
    auto status = AddressColumn::DecodeBatchBlocks(
        column.layout, column.blocks, column.total_records, multi_result);
    ASSERT_TRUE(status.ok());

    auto serial_result = SerialDecode(column);
    ASSERT_EQ(multi_result.size(), serial_result.size());
    for (size_t i = 0; i < multi_result.size(); ++i) {
        EXPECT_EQ(multi_result[i].offset, serial_result[i].offset)
            << "record " << i;
        EXPECT_EQ(multi_result[i].size, serial_result[i].size)
            << "record " << i;
    }
}
