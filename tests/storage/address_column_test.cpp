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
// Encode / Decode round-trip
// ============================================================================

TEST(AddressColumnTest, EmptyInput) {
    std::vector<AddressEntry> entries;
    auto blocks = AddressColumn::Encode(entries);
    EXPECT_TRUE(blocks.empty());
    EXPECT_EQ(AddressColumn::TotalRecords(blocks), 0u);
}

TEST(AddressColumnTest, SingleRecord) {
    std::vector<AddressEntry> entries = {{1000, 256}};
    auto blocks = AddressColumn::Encode(entries, 64, 1);

    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].base_offset, 1000u);
    EXPECT_EQ(blocks[0].record_count, 1u);
    EXPECT_EQ(AddressColumn::TotalRecords(blocks), 1u);

    // Decode
    auto e = AddressColumn::Lookup(blocks, 0);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].record_count, 64u);

    // Verify round-trip for every entry
    for (uint32_t i = 0; i < 64; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].record_count, 64u);
    EXPECT_EQ(blocks[1].record_count, 36u);
    EXPECT_EQ(AddressColumn::TotalRecords(blocks), 100u);

    // Verify all entries
    for (uint32_t i = 0; i < 100; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].bit_width, 8u);  // 128 needs 8 bits

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].bit_width, 2u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_GE(blocks[0].bit_width, 17u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);

    std::vector<AddressEntry> decoded;
    AddressColumn::DecodeBlock(blocks[0], decoded);
    ASSERT_EQ(decoded.size(), 64u);

    for (uint32_t i = 0; i < 64; ++i) {
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].record_count, 10u);

    std::vector<AddressEntry> decoded;
    AddressColumn::DecodeBlock(blocks[0], decoded);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);

    std::vector<uint32_t> indices = {0, 10, 32, 63};
    auto results = AddressColumn::BatchLookup(blocks, indices);

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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    EXPECT_EQ(blocks.size(), 4u);  // 200/64 = 3.125 → 4 blocks

    std::vector<uint32_t> indices = {0, 63, 64, 127, 128, 199};
    auto results = AddressColumn::BatchLookup(blocks, indices);

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
    auto blocks = AddressColumn::Encode(entries, 64, 1);

    auto e = AddressColumn::Lookup(blocks, 999);
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

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    EXPECT_EQ(AddressColumn::TotalRecords(blocks),
              static_cast<uint32_t>(N));

    // Verify every entry
    for (uint32_t i = 0; i < static_cast<uint32_t>(N); ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

// ============================================================================
// Custom granularity
// ============================================================================

TEST(AddressColumnTest, CustomGranularity_32) {
    std::vector<AddressEntry> entries;
    uint64_t off = 0;
    for (int i = 0; i < 100; ++i) {
        entries.push_back({off, 256u});
        off += 256;
    }

    auto blocks = AddressColumn::Encode(entries, 32, 1);
    EXPECT_EQ(blocks.size(), 4u);  // 100/32 = 3.125 → 4

    for (uint32_t i = 0; i < 100; ++i) {
        auto e = AddressColumn::Lookup(blocks, i, 32);
        EXPECT_EQ(e.offset, entries[i].offset) << "record " << i;
        EXPECT_EQ(e.size, entries[i].size) << "record " << i;
    }
}

TEST(AddressColumnTest, ZeroSizes) {
    // Edge case: all sizes = 0
    std::vector<AddressEntry> entries;
    for (int i = 0; i < 64; ++i) {
        entries.push_back({0, 0});
    }

    auto blocks = AddressColumn::Encode(entries, 64, 1);
    ASSERT_EQ(blocks.size(), 1u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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
    auto blocks = AddressColumn::Encode(entries, 64, 4096);

    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].page_size, 4096u);
    EXPECT_EQ(blocks[0].base_offset, 0u);  // page index 0

    auto e = AddressColumn::Lookup(blocks, 0);
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

    auto blocks = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].page_size, page_size);
    // base_offset in page units: 0
    EXPECT_EQ(blocks[0].base_offset, 0u);
    // bit_width: all sizes are 2 pages, needs 2 bits
    EXPECT_EQ(blocks[0].bit_width, 2u);

    for (uint32_t i = 0; i < 10; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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

    auto blocks = AddressColumn::Encode(entries, 64, page_size);
    ASSERT_EQ(blocks.size(), 1u);
    // max pages = 5, needs 3 bits
    EXPECT_EQ(blocks[0].bit_width, 3u);

    for (uint32_t i = 0; i < 64; ++i) {
        auto e = AddressColumn::Lookup(blocks, i);
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
    auto blocks_byte = AddressColumn::Encode(entries, 64, 1);
    EXPECT_EQ(blocks_byte[0].bit_width, 14u);

    // page_size=4096: needs 2 bits (size=2 in page units)
    auto blocks_page = AddressColumn::Encode(entries, 64, page_size);
    EXPECT_EQ(blocks_page[0].bit_width, 2u);

    // Both decode to the same byte addresses
    for (uint32_t i = 0; i < 64; ++i) {
        auto e1 = AddressColumn::Lookup(blocks_byte, i);
        auto e2 = AddressColumn::Lookup(blocks_page, i);
        EXPECT_EQ(e1.offset, e2.offset);
        EXPECT_EQ(e1.size, e2.size);
    }
}
