#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace vdb {

// ============================================================================
// Basic Type Aliases
// ============================================================================

/// Vector ID type (64-bit to support large datasets)
using VecID = uint64_t;

/// Row ID type (same as VecID for consistency)
using RowID = uint64_t;

/// Segment ID type
using SegmentID = uint32_t;

/// List ID for IVF (cluster ID)
using ListID = uint32_t;

/// Dimension type
using Dim = uint32_t;

/// Invalid/sentinel values
constexpr VecID kInvalidVecID = std::numeric_limits<VecID>::max();
constexpr RowID kInvalidRowID = std::numeric_limits<RowID>::max();
constexpr SegmentID kInvalidSegmentID = std::numeric_limits<SegmentID>::max();
constexpr ListID kInvalidListID = std::numeric_limits<ListID>::max();

// ============================================================================
// Distance Metric Types
// ============================================================================

enum class MetricType : uint8_t {
  L2 = 0,           // Euclidean distance (squared)
  InnerProduct = 1,           // Inner product (larger = more similar)
  COSINE = 2,       // Cosine similarity (normalized InnerProduct)
};

inline std::string_view MetricTypeName(MetricType metric) {
  switch (metric) {
    case MetricType::L2: return "L2";
    case MetricType::InnerProduct: return "InnerProduct";
    case MetricType::COSINE: return "COSINE";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// Data Types for Columns
// ============================================================================

enum class DType : uint8_t {
  // Integer types
  INT8 = 0,
  INT16 = 1,
  INT32 = 2,
  INT64 = 3,
  UINT8 = 4,
  UINT16 = 5,
  UINT32 = 6,
  UINT64 = 7,
  
  // Floating point types
  FLOAT16 = 10,
  FLOAT32 = 11,
  FLOAT64 = 12,
  
  // Variable length types
  STRING = 20,
  BYTES = 21,
  
  // Vector types (fixed-length arrays)
  VECTOR_FLOAT32 = 30,
  VECTOR_FLOAT16 = 31,
  VECTOR_INT8 = 32,     // Quantized vectors
  VECTOR_UINT8 = 33,    // PQ codes
  
  // Special
  BOOL = 40,
  TIMESTAMP = 41,
};

/// Get byte size for fixed-width types, returns 0 for variable-length types
inline size_t DTypeSize(DType dtype) {
  switch (dtype) {
    case DType::INT8:
    case DType::UINT8:
    case DType::BOOL:
      return 1;
    case DType::INT16:
    case DType::UINT16:
    case DType::FLOAT16:
      return 2;
    case DType::INT32:
    case DType::UINT32:
    case DType::FLOAT32:
      return 4;
    case DType::INT64:
    case DType::UINT64:
    case DType::FLOAT64:
    case DType::TIMESTAMP:
      return 8;
    // Variable length and vector types
    default:
      return 0;
  }
}

inline bool DTypeIsFixedWidth(DType dtype) {
  return DTypeSize(dtype) > 0;
}

inline bool DTypeIsVector(DType dtype) {
  return dtype == DType::VECTOR_FLOAT32 ||
         dtype == DType::VECTOR_FLOAT16 ||
         dtype == DType::VECTOR_INT8 ||
         dtype == DType::VECTOR_UINT8;
}

inline std::string_view DTypeName(DType dtype) {
  switch (dtype) {
    case DType::INT8: return "INT8";
    case DType::INT16: return "INT16";
    case DType::INT32: return "INT32";
    case DType::INT64: return "INT64";
    case DType::UINT8: return "UINT8";
    case DType::UINT16: return "UINT16";
    case DType::UINT32: return "UINT32";
    case DType::UINT64: return "UINT64";
    case DType::FLOAT16: return "FLOAT16";
    case DType::FLOAT32: return "FLOAT32";
    case DType::FLOAT64: return "FLOAT64";
    case DType::STRING: return "STRING";
    case DType::BYTES: return "BYTES";
    case DType::VECTOR_FLOAT32: return "VECTOR_FLOAT32";
    case DType::VECTOR_FLOAT16: return "VECTOR_FLOAT16";
    case DType::VECTOR_INT8: return "VECTOR_INT8";
    case DType::VECTOR_UINT8: return "VECTOR_UINT8";
    case DType::BOOL: return "BOOL";
    case DType::TIMESTAMP: return "TIMESTAMP";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// Search Result
// ============================================================================

/// Single search result entry
struct SearchResult {
  VecID id;
  float distance;
  
  bool operator<(const SearchResult& other) const {
    return distance < other.distance;
  }
  
  bool operator>(const SearchResult& other) const {
    return distance > other.distance;
  }
};

// ============================================================================
// Constants
// ============================================================================

/// Default block size for I/O alignment (4KB)
constexpr size_t kDefaultBlockSize = 4096;

/// Default page size for mmap
constexpr size_t kDefaultPageSize = 4096;

/// Cache line size for SIMD alignment
constexpr size_t kCacheLineSize = 64;

/// SIMD register width (AVX2 = 256 bits = 32 bytes)
constexpr size_t kSimdWidth = 32;

/// AVX-512 register width
constexpr size_t kSimd512Width = 64;

}  // namespace vdb
