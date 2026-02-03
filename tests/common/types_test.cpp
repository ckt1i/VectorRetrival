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
  EXPECT_EQ(MetricTypeName(MetricType::IP), "IP");
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
}

}  // namespace
}  // namespace vdb
