#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

/// Cluster ID (semantic alias for ListID in new architecture)
using ClusterID = uint32_t;

/// DataFile ID type (1:1 with cluster, reserved for multi-shard expansion)
using FileID = uint16_t;

/// Column ID
using ColumnID = uint32_t;

/// Dimension type
using Dim = uint32_t;

/// Invalid/sentinel values
constexpr VecID kInvalidVecID = std::numeric_limits<VecID>::max();
constexpr RowID kInvalidRowID = std::numeric_limits<RowID>::max();
constexpr SegmentID kInvalidSegmentID = std::numeric_limits<SegmentID>::max();
constexpr ListID kInvalidListID = std::numeric_limits<ListID>::max();
constexpr ClusterID kInvalidClusterID = std::numeric_limits<ClusterID>::max();
constexpr FileID kInvalidFileID = std::numeric_limits<FileID>::max();
constexpr ColumnID kInvalidColumnID = std::numeric_limits<ColumnID>::max();

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
// Search Result (forward declare, will be defined after Datum)
// ============================================================================

// Defined after Datum struct below

// ============================================================================
// Payload Mode (inline vs extern)
// ============================================================================

/// Payload storage mode — small payloads are stored inline in the record file,
/// large payloads (>= kInlineThreshold) are stored in external blob files.
enum class PayloadMode : uint8_t {
  kInline = 0,   // Payload bytes embedded in record
  kExtern = 1,   // Payload stored in external blob file
};

inline std::string_view PayloadModeName(PayloadMode mode) {
  switch (mode) {
    case PayloadMode::kInline: return "Inline";
    case PayloadMode::kExtern: return "Extern";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// External Blob Reference
// ============================================================================

/// Reference to a payload chunk stored in an external blob file.
struct ExternRef {
  uint32_t file_id;   // Blob file identifier (index into FileNameDirectory)
  uint64_t offset;     // Byte offset within the blob file
  uint32_t length;     // Payload byte length

  bool operator==(const ExternRef& other) const {
    return file_id == other.file_id &&
           offset  == other.offset  &&
           length  == other.length;
  }
  bool operator!=(const ExternRef& other) const { return !(*this == other); }
};

// ============================================================================
// Record Locator
// ============================================================================

/// Locates a record inside the segment's record file.
/// ANNS returns (VecID, approx_dist, RecordLocator) so the reader can
/// pread() directly — no row-id → offset translation needed.
struct RecordLocator {
  uint64_t    offset;   // Byte offset in the record file
  uint32_t    length;   // Total serialised record length
  PayloadMode mode;     // Inline or Extern

  bool operator==(const RecordLocator& other) const {
    return offset == other.offset &&
           length == other.length &&
           mode   == other.mode;
  }
  bool operator!=(const RecordLocator& other) const { return !(*this == other); }
};

// ============================================================================
// Column Locator — physical address of one value within a column chunk
// ============================================================================

/// Locates a single cell within a column file.
/// Used as the physical-address token returned by the writer and consumed by
/// the reader (pread-style I/O).
struct ColumnLocator {
  uint32_t chunk_id;           // Which chunk inside the column
  uint64_t data_offset;        // Byte offset of the value within the file
  uint32_t data_length;        // Byte length of the value
  uint32_t offset_table_pos;   // Slot index in the variable-length offset table
                               //   (0 for fixed-width columns)

  bool operator==(const ColumnLocator& other) const {
    return chunk_id        == other.chunk_id &&
           data_offset     == other.data_offset &&
           data_length     == other.data_length &&
           offset_table_pos == other.offset_table_pos;
  }
  bool operator!=(const ColumnLocator& other) const { return !(*this == other); }
};

// ============================================================================
// Column Schema — lightweight column descriptor
// ============================================================================

struct ColumnSchema {
  ColumnID    id;
  std::string name;
  DType       dtype;
  bool        nullable = false;
};

// ============================================================================
// Datum — a single column value (type-erased)
// ============================================================================

/// Holds one cell value.  Fixed-width types are stored inline; variable-length
/// types carry their bytes in the string member.
struct Datum {
  DType dtype;
  union {
    int64_t  i64;
    uint64_t u64;
    double   f64;
    float    f32;
    int32_t  i32;
    uint32_t u32;
    int16_t  i16;
    uint16_t u16;
    int8_t   i8;
    uint8_t  u8;
    bool     b;
  } fixed;
  std::string var_data;   // For STRING / BYTES

  Datum() : dtype(DType::INT64) { fixed.i64 = 0; }

  static Datum Int64(int64_t v)  { Datum d; d.dtype = DType::INT64;  d.fixed.i64 = v; return d; }
  static Datum UInt64(uint64_t v){ Datum d; d.dtype = DType::UINT64; d.fixed.u64 = v; return d; }
  static Datum Int32(int32_t v)  { Datum d; d.dtype = DType::INT32;  d.fixed.i32 = v; return d; }
  static Datum UInt32(uint32_t v){ Datum d; d.dtype = DType::UINT32; d.fixed.u32 = v; return d; }
  static Datum Float32(float v)  { Datum d; d.dtype = DType::FLOAT32;d.fixed.f32 = v; return d; }
  static Datum Float64(double v) { Datum d; d.dtype = DType::FLOAT64;d.fixed.f64 = v; return d; }
  static Datum Bool(bool v)      { Datum d; d.dtype = DType::BOOL;   d.fixed.b   = v; return d; }
  static Datum Timestamp(uint64_t v){ Datum d; d.dtype = DType::TIMESTAMP; d.fixed.u64 = v; return d; }
  static Datum String(std::string v){
    Datum d; d.dtype = DType::STRING; d.var_data = std::move(v); return d;
  }
  static Datum Bytes(std::string v) {
    Datum d; d.dtype = DType::BYTES;  d.var_data = std::move(v); return d;
  }

  /// Returns pointer to the raw bytes of the fixed-width value.
  const void* fixed_data() const { return &fixed; }

  /// Returns byte size of the value (for serialisation).
  size_t byte_size() const {
    if (DTypeIsFixedWidth(dtype)) return DTypeSize(dtype);
    return var_data.size();
  }

  /// Returns pointer to the raw bytes of the value.
  const void* data() const {
    if (DTypeIsFixedWidth(dtype)) return &fixed;
    return var_data.data();
  }
};

// ============================================================================
// Search Result (now that Datum is defined)
// ============================================================================

/// Single search result entry (optionally includes payload columns)
struct SearchResult {
  VecID id;
  float distance;
  std::map<ColumnID, Datum> payload;  // Payload columns indexed by ColumnID
  
  bool operator<(const SearchResult& other) const {
    return distance < other.distance;
  }
  
  bool operator>(const SearchResult& other) const {
    return distance > other.distance;
  }
};

// ============================================================================
// Record Physical Address — locates a full record and its per-column cells
// ============================================================================

/// DEPRECATED: Use AddressEntry instead.
/// Locates a record inside the segment's record file.
struct [[deprecated("Use AddressEntry instead for Phase 2.5+")]] RecordPhysicalAddr {
  RecordLocator                             record;   // Record-file position
  std::vector<std::pair<ColumnID, ColumnLocator>> columns;  // Per-column positions

  bool operator==(const RecordPhysicalAddr& other) const {
    return record == other.record && columns == other.columns;
  }
  bool operator!=(const RecordPhysicalAddr& other) const { return !(*this == other); }
};

// ============================================================================
// New Types for Phase 2.5+ (IVF+ConANN+RaBitQ Architecture)
// ============================================================================

/// Result classification from ConANN (Cluster Architecture Nearest Neighbor Analysis)
enum class ResultClass : uint8_t {
  SafeIn = 0,      // Approx dist < tau_in: definitely in Top-K, fetch all data
  SafeOut = 1,     // Approx dist > tau_out: definitely not in Top-K, skip
  Uncertain = 2,   // tau_in <= approx_dist <= tau_out: need exact distance verification
};

inline std::string_view ResultClassName(ResultClass cls) {
  switch (cls) {
    case ResultClass::SafeIn: return "SafeIn";
    case ResultClass::SafeOut: return "SafeOut";
    case ResultClass::Uncertain: return "Uncertain";
    default: return "UNKNOWN";
  }
}

/// Physical address of a record in DataFile (offset + size)
struct AddressEntry {
  uint64_t offset;  // Byte offset in DataFile
  uint32_t size;    // Record byte length

  bool operator==(const AddressEntry& other) const {
    return offset == other.offset && size == other.size;
  }
  bool operator!=(const AddressEntry& other) const { return !(*this == other); }
};

/// I/O task type for query pipeline read classification
enum class ReadTaskType : uint8_t {
  VEC_ONLY = 0,    // Read only vector part (dim * sizeof(float) bytes)
  ALL = 1,         // Read entire record (vec + payload, for SafeIn ≤256KB)
  PAYLOAD = 2,     // Read only payload part (from offset + dim*4)
};

inline std::string_view ReadTaskTypeName(ReadTaskType ty) {
  switch (ty) {
    case ReadTaskType::VEC_ONLY: return "VEC_ONLY";
    case ReadTaskType::ALL: return "ALL";
    case ReadTaskType::PAYLOAD: return "PAYLOAD";
    default: return "UNKNOWN";
  }
}

/// Candidate vector for progressive reranking
struct Candidate {
  float approx_dist;         // RaBitQ estimated distance
  ResultClass result_class;  // ConANN classification
  ClusterID cluster_id;      // Source cluster
  uint32_t local_idx;        // Local index in cluster

  bool operator<(const Candidate& other) const {
    // For max-heap: larger distance is "less than"
    return approx_dist > other.approx_dist;
  }
  bool operator>(const Candidate& other) const {
    return approx_dist < other.approx_dist;
  }
};

/// RaBitQ (Reduced-Bit Quantization) configuration
struct RaBitQConfig {
  uint8_t bits = 1;          // Quantization bits: 1, 2, 4, or 8
  uint32_t block_size = 64;  // Block granularity for SIMD (typically 64)
  float c_factor = 5.75f;    // Error bound factor: epsilon = c * 2^(-B/2) / sqrt(D)
  uint8_t storage_version = 7;  // On-disk format version (7 = dual-region FastScan)

  bool operator==(const RaBitQConfig& other) const {
    return bits == other.bits &&
           block_size == other.block_size &&
           c_factor == other.c_factor &&
           storage_version == other.storage_version;
  }
  bool operator!=(const RaBitQConfig& other) const { return !(*this == other); }
};

// ============================================================================
// Constants
// ============================================================================

/// Default block size for I/O alignment (4KB)
constexpr size_t kDefaultBlockSize = 4096;

/// Default page size for page-aligned record storage (4KB)
constexpr uint32_t kDefaultPageSize = 4096;

/// Cache line size for SIMD alignment
constexpr size_t kCacheLineSize = 64;

/// SIMD register width (AVX2 = 256 bits = 32 bytes)
constexpr size_t kSimdWidth = 32;

/// AVX-512 register width
constexpr size_t kSimd512Width = 64;

/// Inline/Extern threshold: payloads smaller than this are stored inline
/// in the record file; payloads >= this size go to an external blob file.
constexpr size_t kInlineThreshold = 4096;

}  // namespace vdb
