// =============================================================================
// ColumnChunkWriter / ColumnChunkReader Tests
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>

#include "vdb/columns/column_chunk_writer.h"
#include "vdb/columns/column_chunk_reader.h"
#include "vdb/common/types.h"
#include "vdb/common/aligned_alloc.h"

namespace vdb {
namespace {

// =============================================================================
// Fixed-Width Column Tests
// =============================================================================

class FixedWidthColumnTest : public ::testing::Test {
 protected:
  ColumnSchema int64_schema{0, "test_int64", DType::INT64, false};
};

TEST_F(FixedWidthColumnTest, AppendSingleValue) {
  ColumnChunkWriter writer(int64_schema, /*chunk_id=*/0, /*base_offset=*/0);

  int64_t val = 42;
  auto loc_result = writer.Append(&val, sizeof(val));
  ASSERT_TRUE(loc_result.ok()) << loc_result.status().ToString();

  ColumnLocator loc = *loc_result;
  EXPECT_EQ(loc.chunk_id, 0);
  EXPECT_EQ(loc.data_offset, 0);
  EXPECT_EQ(loc.data_length, 8);
  EXPECT_EQ(loc.offset_table_pos, 0);  // unused for fixed-width

  EXPECT_EQ(writer.num_records(), 1);
  EXPECT_EQ(writer.data_size(), 8);
  EXPECT_EQ(writer.offset_table_size(), 0);
}

TEST_F(FixedWidthColumnTest, AppendMultipleValues) {
  ColumnChunkWriter writer(int64_schema, /*chunk_id=*/1, /*base_offset=*/1000);

  std::vector<int64_t> values = {10, 20, 30, 40, 50};
  std::vector<ColumnLocator> locs;

  for (int64_t v : values) {
    auto loc_result = writer.Append(&v, sizeof(v));
    ASSERT_TRUE(loc_result.ok());
    locs.push_back(*loc_result);
  }

  EXPECT_EQ(writer.num_records(), 5);
  EXPECT_EQ(writer.data_size(), 40);

  // Check locators.
  for (size_t i = 0; i < locs.size(); ++i) {
    EXPECT_EQ(locs[i].chunk_id, 1);
    EXPECT_EQ(locs[i].data_offset, 1000 + i * 8);
    EXPECT_EQ(locs[i].data_length, 8);
  }
}

TEST_F(FixedWidthColumnTest, WriteAndReadBack) {
  ColumnChunkWriter writer(int64_schema, /*chunk_id=*/0, /*base_offset=*/0);

  std::vector<int64_t> values = {100, 200, 300};
  std::vector<ColumnLocator> locs;

  for (int64_t v : values) {
    auto loc_result = writer.Append(&v, sizeof(v));
    ASSERT_TRUE(loc_result.ok());
    locs.push_back(*loc_result);
  }

  // Serialise.
  auto buf_result = writer.Serialise();
  ASSERT_TRUE(buf_result.ok());
  std::vector<uint8_t> buf = std::move(*buf_result);
  EXPECT_EQ(buf.size(), 24);

  // Read back.
  ColumnChunkReader reader(buf.data(), buf.size(), int64_schema);

  for (size_t i = 0; i < values.size(); ++i) {
    auto datum_result = reader.ReadDatum(locs[i]);
    ASSERT_TRUE(datum_result.ok()) << datum_result.status().ToString();
    Datum datum = *datum_result;
    EXPECT_EQ(datum.dtype, DType::INT64);
    EXPECT_EQ(datum.fixed.i64, values[i]);
  }
}

// =============================================================================
// Variable-Length Column Tests
// =============================================================================

class VarLenColumnTest : public ::testing::Test {
 protected:
  ColumnSchema string_schema{1, "test_string", DType::STRING, false};
};

TEST_F(VarLenColumnTest, AppendSingleString) {
  ColumnChunkWriter writer(string_schema, /*chunk_id=*/0, /*base_offset=*/0);

  std::string val = "hello";
  auto loc_result = writer.Append(val.data(), val.size());
  ASSERT_TRUE(loc_result.ok());

  ColumnLocator loc = *loc_result;
  EXPECT_EQ(loc.chunk_id, 0);
  EXPECT_EQ(loc.data_offset, 0);
  EXPECT_EQ(loc.data_length, 5);
  EXPECT_EQ(loc.offset_table_pos, 0);

  EXPECT_EQ(writer.num_records(), 1);
  EXPECT_EQ(writer.data_size(), 5);
  // offset table: [0, 5]  → 2 entries × 8 bytes = 16 bytes
  EXPECT_EQ(writer.offset_table_size(), 16);
}

TEST_F(VarLenColumnTest, AppendMultipleStrings) {
  ColumnChunkWriter writer(string_schema, /*chunk_id=*/0, /*base_offset=*/100);

  std::vector<std::string> values = {"foo", "barbaz", "", "x"};
  std::vector<ColumnLocator> locs;

  for (const auto& v : values) {
    auto loc_result = writer.Append(v.data(), v.size());
    ASSERT_TRUE(loc_result.ok());
    locs.push_back(*loc_result);
  }

  EXPECT_EQ(writer.num_records(), 4);
  // total string bytes: 3 + 6 + 0 + 1 = 10
  EXPECT_EQ(writer.data_size(), 10);
  // offset table: [0, 3, 9, 9, 10] → 5 entries × 8 bytes = 40 bytes
  EXPECT_EQ(writer.offset_table_size(), 40);

  // Check locators.
  EXPECT_EQ(locs[0].data_offset, 100 + 0);
  EXPECT_EQ(locs[0].data_length, 3);
  EXPECT_EQ(locs[0].offset_table_pos, 0);

  EXPECT_EQ(locs[1].data_offset, 100 + 3);
  EXPECT_EQ(locs[1].data_length, 6);
  EXPECT_EQ(locs[1].offset_table_pos, 1);

  EXPECT_EQ(locs[2].data_offset, 100 + 9);
  EXPECT_EQ(locs[2].data_length, 0);
  EXPECT_EQ(locs[2].offset_table_pos, 2);

  EXPECT_EQ(locs[3].data_offset, 100 + 9);
  EXPECT_EQ(locs[3].data_length, 1);
  EXPECT_EQ(locs[3].offset_table_pos, 3);
}

TEST_F(VarLenColumnTest, WriteAndReadBack) {
  ColumnChunkWriter writer(string_schema, /*chunk_id=*/0, /*base_offset=*/0);

  std::vector<std::string> values = {"alpha", "beta", "gamma"};
  std::vector<ColumnLocator> locs;

  for (const auto& v : values) {
    auto loc_result = writer.Append(v.data(), v.size());
    ASSERT_TRUE(loc_result.ok());
    locs.push_back(*loc_result);
  }

  // Serialise.
  auto buf_result = writer.Serialise();
  ASSERT_TRUE(buf_result.ok());
  std::vector<uint8_t> buf = std::move(*buf_result);

  // Read back.
  ColumnChunkReader reader(buf.data(), buf.size(), string_schema);

  for (size_t i = 0; i < values.size(); ++i) {
    auto datum_result = reader.ReadDatum(locs[i]);
    ASSERT_TRUE(datum_result.ok()) << datum_result.status().ToString();
    Datum datum = *datum_result;
    EXPECT_EQ(datum.dtype, DType::STRING);
    EXPECT_EQ(datum.var_data, values[i]);
  }
}

// =============================================================================
// Datum-based Append Tests
// =============================================================================

TEST(DatumAppendTest, AppendDatumInt64) {
  ColumnSchema schema{0, "c", DType::INT64, false};
  ColumnChunkWriter writer(schema, 0, 0);

  Datum d = Datum::Int64(-999);
  auto loc_result = writer.Append(d);
  ASSERT_TRUE(loc_result.ok());

  auto buf_result = writer.Serialise();
  ASSERT_TRUE(buf_result.ok());

  ColumnChunkReader reader(buf_result->data(), buf_result->size(), schema);
  auto read_result = reader.ReadDatum(*loc_result);
  ASSERT_TRUE(read_result.ok());
  EXPECT_EQ(read_result->fixed.i64, -999);
}

TEST(DatumAppendTest, AppendDatumString) {
  ColumnSchema schema{1, "s", DType::STRING, false};
  ColumnChunkWriter writer(schema, 0, 0);

  Datum d = Datum::String("test string value");
  auto loc_result = writer.Append(d);
  ASSERT_TRUE(loc_result.ok());

  auto buf_result = writer.Serialise();
  ASSERT_TRUE(buf_result.ok());

  ColumnChunkReader reader(buf_result->data(), buf_result->size(), schema);
  auto read_result = reader.ReadDatum(*loc_result);
  ASSERT_TRUE(read_result.ok());
  EXPECT_EQ(read_result->var_data, "test string value");
}

// =============================================================================
// Batch Read Test
// =============================================================================

TEST(BatchReadTest, ReadBatchFixedWidth) {
  ColumnSchema schema{0, "x", DType::INT32, false};
  ColumnChunkWriter writer(schema, 0, 0);

  std::vector<int32_t> values = {1, 2, 3, 4, 5};
  std::vector<ColumnLocator> locs;

  for (int32_t v : values) {
    auto r = writer.Append(&v, sizeof(v));
    ASSERT_TRUE(r.ok());
    locs.push_back(*r);
  }

  auto buf_result = writer.Serialise();
  ASSERT_TRUE(buf_result.ok());

  ColumnChunkReader reader(buf_result->data(), buf_result->size(), schema);

  std::vector<AlignedBuffer> out;
  auto status = reader.ReadBatch(locs, &out);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(out.size(), 5);

  for (size_t i = 0; i < out.size(); ++i) {
    int32_t val;
    std::memcpy(&val, out[i].data(), sizeof(val));
    EXPECT_EQ(val, values[i]);
  }
}

}  // namespace
}  // namespace vdb
