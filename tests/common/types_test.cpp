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
  SearchResult a{1, 0.5f, {}};
  SearchResult b{2, 1.0f, {}};
  
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  EXPECT_TRUE(b > a);
  EXPECT_FALSE(a > b);
}

TEST(TypesTest, SearchResultWithPayload) {
  SearchResult result;
  result.id = 42;
  result.distance = 1.5f;
  result.payload[0] = Datum::Int64(100);
  result.payload[1] = Datum::String("hello");
  
  EXPECT_EQ(result.id, 42);
  EXPECT_EQ(result.payload.size(), 2);
  EXPECT_EQ(result.payload[0].dtype, DType::INT64);
  EXPECT_EQ(result.payload[0].fixed.i64, 100);
  EXPECT_EQ(result.payload[1].dtype, DType::STRING);
  EXPECT_EQ(result.payload[1].var_data, "hello");
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

// ============================================================================
// Phase 2.5: New type aliases and sentinel values
// ============================================================================

TEST(TypesTest, ClusterIDAlias) {
  ClusterID cid = 42;
  EXPECT_EQ(cid, 42u);
  EXPECT_EQ(kInvalidClusterID, std::numeric_limits<ClusterID>::max());
}

TEST(TypesTest, FileIDAlias) {
  FileID fid = 1;
  EXPECT_EQ(fid, 1u);
  EXPECT_EQ(kInvalidFileID, std::numeric_limits<FileID>::max());
  // FileID is uint16_t
  EXPECT_EQ(sizeof(FileID), 2);
}

// ============================================================================
// Phase 2.5: ResultClass enum
// ============================================================================

TEST(TypesTest, ResultClassValues) {
  EXPECT_EQ(static_cast<uint8_t>(ResultClass::SafeIn), 0);
  EXPECT_EQ(static_cast<uint8_t>(ResultClass::SafeOut), 1);
  EXPECT_EQ(static_cast<uint8_t>(ResultClass::Uncertain), 2);
}

TEST(TypesTest, ResultClassName) {
  EXPECT_EQ(ResultClassName(ResultClass::SafeIn), "SafeIn");
  EXPECT_EQ(ResultClassName(ResultClass::SafeOut), "SafeOut");
  EXPECT_EQ(ResultClassName(ResultClass::Uncertain), "Uncertain");
}

// ============================================================================
// Phase 2.5: AddressEntry
// ============================================================================

TEST(TypesTest, AddressEntryEquality) {
  AddressEntry a{1024, 256};
  AddressEntry b{1024, 256};
  AddressEntry c{2048, 256};
  
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, AddressEntryFields) {
  AddressEntry addr{65536, 4096};
  EXPECT_EQ(addr.offset, 65536u);
  EXPECT_EQ(addr.size, 4096u);
}

// ============================================================================
// Phase 2.5: ReadTaskType enum
// ============================================================================

TEST(TypesTest, ReadTaskTypeValues) {
  EXPECT_EQ(static_cast<uint8_t>(ReadTaskType::VEC_ONLY), 0);
  EXPECT_EQ(static_cast<uint8_t>(ReadTaskType::ALL), 1);
  EXPECT_EQ(static_cast<uint8_t>(ReadTaskType::PAYLOAD), 2);
}

TEST(TypesTest, ReadTaskTypeName) {
  EXPECT_EQ(ReadTaskTypeName(ReadTaskType::VEC_ONLY), "VEC_ONLY");
  EXPECT_EQ(ReadTaskTypeName(ReadTaskType::ALL), "ALL");
  EXPECT_EQ(ReadTaskTypeName(ReadTaskType::PAYLOAD), "PAYLOAD");
}

// ============================================================================
// Phase 2.5: Candidate
// ============================================================================

TEST(TypesTest, CandidateOrdering) {
  // Candidate uses reverse ordering for max-heap (larger dist = "less than")
  Candidate near{0.5f, ResultClass::SafeIn, 0, 0};
  Candidate far{2.0f, ResultClass::Uncertain, 1, 5};

  // near has smaller dist → larger in max-heap sense
  EXPECT_TRUE(far < near);   // far.approx_dist > near.approx_dist → far < near
  EXPECT_TRUE(near > far);
}

TEST(TypesTest, CandidateFields) {
  Candidate cand{1.5f, ResultClass::SafeIn, 42, 7};
  EXPECT_FLOAT_EQ(cand.approx_dist, 1.5f);
  EXPECT_EQ(cand.result_class, ResultClass::SafeIn);
  EXPECT_EQ(cand.cluster_id, 42u);
  EXPECT_EQ(cand.local_idx, 7u);
}

// ============================================================================
// Phase 2.5: RaBitQConfig
// ============================================================================

TEST(TypesTest, RaBitQConfigDefaults) {
  RaBitQConfig config;
  EXPECT_EQ(config.bits, 1);
  EXPECT_EQ(config.block_size, 64u);
  EXPECT_FLOAT_EQ(config.c_factor, 5.75f);
}

TEST(TypesTest, RaBitQConfigEquality) {
  RaBitQConfig a{1, 64, 5.75f};
  RaBitQConfig b{1, 64, 5.75f};
  RaBitQConfig c{2, 64, 5.75f};
  
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, RaBitQConfigCustom) {
  RaBitQConfig config{4, 128, 3.0f};
  EXPECT_EQ(config.bits, 4);
  EXPECT_EQ(config.block_size, 128u);
  EXPECT_FLOAT_EQ(config.c_factor, 3.0f);
}

}  // namespace
}  // namespace vdb
