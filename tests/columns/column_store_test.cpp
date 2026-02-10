// =============================================================================
// ColumnStore Tests — Ingest 1000 records, read back by physical address
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "vdb/columns/column_store.h"
#include "vdb/common/types.h"

namespace vdb {
namespace {

// Column IDs.
constexpr ColumnID kIdColumn   = 0;
constexpr ColumnID kNameColumn = 1;

// =============================================================================
// Basic ColumnStore Tests
// =============================================================================

class ColumnStoreBasicTest : public ::testing::Test {
 protected:
  std::vector<ColumnSchema> schemas_{
      {kIdColumn, "id", DType::INT64, false},
      {kNameColumn, "name", DType::STRING, false},
  };
};

TEST_F(ColumnStoreBasicTest, OpenAndIngestSingleRow) {
  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas_).ok());

  std::map<ColumnID, Datum> row;
  row[kIdColumn]   = Datum::Int64(100);
  row[kNameColumn] = Datum::String("Alice");

  auto result = store.Ingest(row);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(result->column_locs.size(), 2);
  EXPECT_EQ(store.num_rows(), 1);
}

TEST_F(ColumnStoreBasicTest, SerialiseAndOpenForRead) {
  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas_).ok());

  std::map<ColumnID, Datum> row;
  row[kIdColumn]   = Datum::Int64(42);
  row[kNameColumn] = Datum::String("Bob");

  auto ingest_result = store.Ingest(row);
  ASSERT_TRUE(ingest_result.ok());

  // Serialise.
  auto buf_result = store.SerialiseAll();
  ASSERT_TRUE(buf_result.ok());

  // Open for read.
  ColumnStore reader;
  ASSERT_TRUE(reader.OpenForRead(buf_result->data(),
                                 buf_result->size(),
                                 schemas_).ok());

  // Read back.
  auto read_result = reader.Read(ingest_result->column_locs);
  ASSERT_TRUE(read_result.ok());

  EXPECT_EQ(read_result->at(kIdColumn).fixed.i64, 42);
  EXPECT_EQ(read_result->at(kNameColumn).var_data, "Bob");
}

// =============================================================================
// 1000 Records Test (Verification)
// =============================================================================

TEST(ColumnStore1000RecordsTest, IngestAndVerify) {
  constexpr int kNumRecords = 1000;

  std::vector<ColumnSchema> schemas{
      {kIdColumn, "id", DType::INT64, false},
      {kNameColumn, "name", DType::STRING, false},
  };

  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas).ok());

  // Store locators for verification.
  std::vector<IngestResult> all_results;
  all_results.reserve(kNumRecords);

  // Ingest 1000 records.
  for (int i = 0; i < kNumRecords; ++i) {
    std::map<ColumnID, Datum> row;
    row[kIdColumn]   = Datum::Int64(static_cast<int64_t>(i * 100));
    row[kNameColumn] = Datum::String("record_" + std::to_string(i));

    auto result = store.Ingest(row);
    ASSERT_TRUE(result.ok()) << "Failed at row " << i << ": "
                             << result.status().ToString();
    all_results.push_back(std::move(*result));
  }

  EXPECT_EQ(store.num_rows(), kNumRecords);

  // Serialise.
  auto buf_result = store.SerialiseAll();
  ASSERT_TRUE(buf_result.ok());

  // Open for read.
  ColumnStore reader;
  ASSERT_TRUE(reader.OpenForRead(buf_result->data(),
                                 buf_result->size(),
                                 schemas).ok());

  // Verify each record.
  for (int i = 0; i < kNumRecords; ++i) {
    auto read_result = reader.Read(all_results[i].column_locs);
    ASSERT_TRUE(read_result.ok()) << "Failed reading row " << i << ": "
                                  << read_result.status().ToString();

    int64_t expected_id = static_cast<int64_t>(i * 100);
    std::string expected_name = "record_" + std::to_string(i);

    EXPECT_EQ(read_result->at(kIdColumn).fixed.i64, expected_id)
        << "Mismatch at row " << i;
    EXPECT_EQ(read_result->at(kNameColumn).var_data, expected_name)
        << "Mismatch at row " << i;
  }
}

// =============================================================================
// Projection Push-Down Test
// =============================================================================

TEST(ColumnStoreProjectionTest, ReadOnlyRequestedColumns) {
  std::vector<ColumnSchema> schemas{
      {0, "a", DType::INT64, false},
      {1, "b", DType::INT64, false},
      {2, "c", DType::STRING, false},
  };

  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas).ok());

  std::map<ColumnID, Datum> row;
  row[0] = Datum::Int64(10);
  row[1] = Datum::Int64(20);
  row[2] = Datum::String("thirty");

  auto ingest_result = store.Ingest(row);
  ASSERT_TRUE(ingest_result.ok());

  auto buf_result = store.SerialiseAll();
  ASSERT_TRUE(buf_result.ok());

  ColumnStore reader;
  ASSERT_TRUE(reader.OpenForRead(buf_result->data(),
                                 buf_result->size(),
                                 schemas).ok());

  // Read only columns 0 and 2.
  auto read_result = reader.ReadColumns(
      ingest_result->column_locs,
      {0, 2});
  ASSERT_TRUE(read_result.ok());

  EXPECT_EQ(read_result->size(), 2);
  EXPECT_EQ(read_result->at(0).fixed.i64, 10);
  EXPECT_EQ(read_result->at(2).var_data, "thirty");
  // Column 1 should not be present.
  EXPECT_EQ(read_result->find(1), read_result->end());
}

// =============================================================================
// File I/O Test (Flush and OpenForRead from file)
// =============================================================================

TEST(ColumnStoreFileIOTest, FlushAndReadFromFile) {
  std::vector<ColumnSchema> schemas{
      {kIdColumn, "id", DType::INT64, false},
      {kNameColumn, "name", DType::STRING, false},
  };

  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas).ok());

  // Ingest a few rows.
  std::vector<IngestResult> results;
  for (int i = 0; i < 10; ++i) {
    std::map<ColumnID, Datum> row;
    row[kIdColumn]   = Datum::Int64(i);
    row[kNameColumn] = Datum::String("name_" + std::to_string(i));
    auto r = store.Ingest(row);
    ASSERT_TRUE(r.ok());
    results.push_back(std::move(*r));
  }

  // Flush to file.
  const std::string path = "/tmp/vdb_column_store_test.cols";
  auto flush_result = store.Flush(path);
  ASSERT_TRUE(flush_result.ok()) << flush_result.status().ToString();

  // Open from file.
  ColumnStore reader;
  // Set schemas first (required for file-based OpenForRead).
  ASSERT_TRUE(reader.Open(schemas).ok());
  ASSERT_TRUE(reader.OpenForRead(path).ok());

  // Verify.
  for (int i = 0; i < 10; ++i) {
    auto read_result = reader.Read(results[i].column_locs);
    ASSERT_TRUE(read_result.ok());
    EXPECT_EQ(read_result->at(kIdColumn).fixed.i64, i);
    EXPECT_EQ(read_result->at(kNameColumn).var_data,
              "name_" + std::to_string(i));
  }

  // Cleanup.
  std::remove(path.c_str());
}

// =============================================================================
// Empty String Edge Case
// =============================================================================

TEST(ColumnStoreEdgeCasesTest, EmptyStrings) {
  std::vector<ColumnSchema> schemas{
      {0, "s", DType::STRING, false},
  };

  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas).ok());

  std::vector<IngestResult> results;
  std::vector<std::string> values = {"", "a", "", "bc", ""};

  for (const auto& v : values) {
    std::map<ColumnID, Datum> row;
    row[0] = Datum::String(v);
    auto r = store.Ingest(row);
    ASSERT_TRUE(r.ok());
    results.push_back(std::move(*r));
  }

  auto buf_result = store.SerialiseAll();
  ASSERT_TRUE(buf_result.ok());

  ColumnStore reader;
  ASSERT_TRUE(reader.OpenForRead(buf_result->data(),
                                 buf_result->size(),
                                 schemas).ok());

  for (size_t i = 0; i < values.size(); ++i) {
    auto read_result = reader.Read(results[i].column_locs);
    ASSERT_TRUE(read_result.ok());
    EXPECT_EQ(read_result->at(0).var_data, values[i]);
  }
}

// =============================================================================
// Large Values Test
// =============================================================================

TEST(ColumnStoreLargeValuesTest, LargeStrings) {
  std::vector<ColumnSchema> schemas{
      {0, "data", DType::STRING, false},
  };

  ColumnStore store;
  ASSERT_TRUE(store.Open(schemas).ok());

  // Create a few large strings.
  std::vector<std::string> values;
  std::vector<IngestResult> results;

  for (int i = 0; i < 10; ++i) {
    std::string large(10000 + i * 1000, static_cast<char>('A' + i));
    values.push_back(large);

    std::map<ColumnID, Datum> row;
    row[0] = Datum::String(large);
    auto r = store.Ingest(row);
    ASSERT_TRUE(r.ok());
    results.push_back(std::move(*r));
  }

  auto buf_result = store.SerialiseAll();
  ASSERT_TRUE(buf_result.ok());

  ColumnStore reader;
  ASSERT_TRUE(reader.OpenForRead(buf_result->data(),
                                 buf_result->size(),
                                 schemas).ok());

  for (size_t i = 0; i < values.size(); ++i) {
    auto read_result = reader.Read(results[i].column_locs);
    ASSERT_TRUE(read_result.ok());
    EXPECT_EQ(read_result->at(0).var_data, values[i]);
  }
}

}  // namespace
}  // namespace vdb
