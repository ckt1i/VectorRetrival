#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/common/status.h"
#include "vdb/common/aligned_alloc.h"

namespace vdb {

// ============================================================================
// ColumnChunkReader
// ============================================================================
// Reads individual cell values from a column chunk by physical address
// (ColumnLocator).  Supports two operation modes:
//
//   1. File-backed  — the reader calls pread() against an open fd.
//   2. Memory-backed — the reader works on an in-memory byte span (tests,
//      mmap, or when the chunk is already loaded).
//
// Both paths bypass any row-id lookup; the caller supplies the ColumnLocator
// that was returned by ColumnChunkWriter::Append().
// ============================================================================

class ColumnChunkReader {
 public:
  /// Construct a reader over an in-memory buffer.
  /// @param data       Pointer to the serialised chunk (data + offset table).
  /// @param data_size  Total byte size of the chunk.
  /// @param schema     Column descriptor.
  /// The caller must keep `data` alive for the lifetime of this reader.
  ColumnChunkReader(const uint8_t* data,
                    size_t data_size,
                    const ColumnSchema& schema,
                    uint64_t base_offset = 0);

  /// Construct a reader backed by a file descriptor.
  /// Reads are done via pread() — the fd must remain open.
  ColumnChunkReader(int fd,
                    const ColumnSchema& schema,
                    uint64_t base_offset = 0);

  ~ColumnChunkReader();

  // Non-copyable, movable.
  ColumnChunkReader(const ColumnChunkReader&) = delete;
  ColumnChunkReader& operator=(const ColumnChunkReader&) = delete;
  ColumnChunkReader(ColumnChunkReader&&) noexcept;
  ColumnChunkReader& operator=(ColumnChunkReader&&) noexcept;

  /// Read a single value at the position described by `loc`.
  /// Returns an AlignedBuffer containing the raw bytes.
  StatusOr<AlignedBuffer> ReadAt(const ColumnLocator& loc) const;

  /// Read a batch of values.  Results are returned in `out`, one buffer per
  /// locator, in the same order as `locs`.
  Status ReadBatch(const std::vector<ColumnLocator>& locs,
                   std::vector<AlignedBuffer>* out) const;

  /// Read a single value into a Datum.
  StatusOr<Datum> ReadDatum(const ColumnLocator& loc) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vdb
