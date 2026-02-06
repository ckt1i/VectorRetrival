#include <gtest/gtest.h>
#include "vdb/common/types.h"

namespace vdb {
namespace {

TEST(TypesTest, InvalidSentinelValues) {
  EXPECT_EQ(kInvalidVecID, std::numeric_limits<VecID>::max());
  EXPECT_EQ(kInvalidRowID, std::numeric_limits<RowID>::max());
  EXPECT_EQ(kInvalidSegmentID, std::numeric_limits<SegmentID>::max());
  EXPECT_EQ(kInvalidListID, std::numeric_limits<ListID>::max());
}

TEST(TypesTest, MetricTypeName) {
  EXPECT_EQ(MetricTypeName(MetricType::L2), "L2");
  EXPECT_EQ(MetricTypeName(MetricType::InnerProduct), "InnerProduct");
  EXPECT_EQ(MetricTypeName(MetricType::COSINE), "COSINE");
}

TEST(TypesTest, DTypeSize) {
  // Fixed-width types
  EXPECT_EQ(DTypeSize(DType::INT8), 1);
  EXPECT_EQ(DTypeSize(DType::INT16), 2);
  EXPECT_EQ(DTypeSize(DType::INT32), 4);
  EXPECT_EQ(DTypeSize(DType::INT64), 8);
  EXPECT_EQ(DTypeSize(DType::UINT8), 1);
  EXPECT_EQ(DTypeSize(DType::UINT16), 2);
  EXPECT_EQ(DTypeSize(DType::UINT32), 4);
  EXPECT_EQ(DTypeSize(DType::UINT64), 8);
  EXPECT_EQ(DTypeSize(DType::FLOAT16), 2);
  EXPECT_EQ(DTypeSize(DType::FLOAT32), 4);
  EXPECT_EQ(DTypeSize(DType::FLOAT64), 8);
  EXPECT_EQ(DTypeSize(DType::BOOL), 1);
  EXPECT_EQ(DTypeSize(DType::TIMESTAMP), 8);
  
  // Variable-width types return 0
  EXPECT_EQ(DTypeSize(DType::STRING), 0);
  EXPECT_EQ(DTypeSize(DType::BYTES), 0);
  EXPECT_EQ(DTypeSize(DType::VECTOR_FLOAT32), 0);
}

TEST(TypesTest, DTypeIsFixedWidth) {
  EXPECT_TRUE(DTypeIsFixedWidth(DType::INT32));
  EXPECT_TRUE(DTypeIsFixedWidth(DType::FLOAT32));
  EXPECT_FALSE(DTypeIsFixedWidth(DType::STRING));
  EXPECT_FALSE(DTypeIsFixedWidth(DType::BYTES));
  EXPECT_FALSE(DTypeIsFixedWidth(DType::VECTOR_FLOAT32));
}

TEST(TypesTest, DTypeIsVector) {
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_FLOAT32));
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_FLOAT16));
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_INT8));
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_UINT8));
  
  EXPECT_FALSE(DTypeIsVector(DType::INT32));
  EXPECT_FALSE(DTypeIsVector(DType::FLOAT32));
  EXPECT_FALSE(DTypeIsVector(DType::STRING));
}

TEST(TypesTest, DTypeName) {
  EXPECT_EQ(DTypeName(DType::INT32), "INT32");
  EXPECT_EQ(DTypeName(DType::FLOAT32), "FLOAT32");
  EXPECT_EQ(DTypeName(DType::STRING), "STRING");
  EXPECT_EQ(DTypeName(DType::VECTOR_FLOAT32), "VECTOR_FLOAT32");
}

TEST(TypesTest, SearchResultComparison) {
  SearchResult a{1, 0.5f};
  SearchResult b{2, 1.0f};
  
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  EXPECT_TRUE(b > a);
  EXPECT_FALSE(a > b);
}

TEST(TypesTest, Constants) {
  EXPECT_EQ(kDefaultBlockSize, 4096);
  EXPECT_EQ(kDefaultPageSize, 4096);
  EXPECT_EQ(kCacheLineSize, 64);
  EXPECT_EQ(kSimdWidth, 32);
  EXPECT_EQ(kSimd512Width, 64);
  EXPECT_EQ(kInlineThreshold, 4096);
}

// ============================================================================
// PayloadMode tests
// ============================================================================

TEST(TypesTest, PayloadModeName) {
  EXPECT_EQ(PayloadModeName(PayloadMode::kInline), "Inline");
  EXPECT_EQ(PayloadModeName(PayloadMode::kExtern), "Extern");
}

TEST(TypesTest, PayloadModeValues) {
  EXPECT_EQ(static_cast<uint8_t>(PayloadMode::kInline), 0);
  EXPECT_EQ(static_cast<uint8_t>(PayloadMode::kExtern), 1);
}

// ============================================================================
// ExternRef tests
// ============================================================================

TEST(TypesTest, ExternRefEquality) {
  ExternRef a{1, 4096, 2048};
  ExternRef b{1, 4096, 2048};
  ExternRef c{2, 4096, 2048};

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, ExternRefFields) {
  ExternRef ref{42, 1024000, 8192};
  EXPECT_EQ(ref.file_id, 42);
  EXPECT_EQ(ref.offset, 1024000);
  EXPECT_EQ(ref.length, 8192);
}

// ============================================================================
// RecordLocator tests
// ============================================================================

TEST(TypesTest, RecordLocatorEquality) {
  RecordLocator a{0, 512, PayloadMode::kInline};
  RecordLocator b{0, 512, PayloadMode::kInline};
  RecordLocator c{0, 512, PayloadMode::kExtern};

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, RecordLocatorFields) {
  RecordLocator loc{65536, 4000, PayloadMode::kInline};
  EXPECT_EQ(loc.offset, 65536);
  EXPECT_EQ(loc.length, 4000);
  EXPECT_EQ(loc.mode, PayloadMode::kInline);
}

TEST(TypesTest, InlineThresholdDecision) {
  // Payloads < kInlineThreshold => inline
  EXPECT_TRUE(100 < kInlineThreshold);
  EXPECT_TRUE(4095 < kInlineThreshold);
  // Payloads >= kInlineThreshold => extern
  EXPECT_FALSE(4096 < kInlineThreshold);
  EXPECT_FALSE(10000 < kInlineThreshold);
}

}  // namespace
}  // namespace vdb
