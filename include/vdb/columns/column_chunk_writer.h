#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

#include "vdb/common/types.h"
#include "vdb/common/status.h"

namespace vdb {

// ============================================================================
// ColumnChunkWriter
// ============================================================================
// Writes a single column's data into an in-memory buffer, then flushes to disk.
//
// Two encoding paths:
//   1. Fixed-width (RawEncoding) — values packed end-to-end.
//   2. Variable-length (OffsetIndex) — an offset table + data region.
//
// Every Append() returns a ColumnLocator giving the physical position of the
// value just written.  The caller stores these locators alongside the vector
// index so that reads go through physical addresses (no row-id lookup).
// ============================================================================

class ColumnChunkWriter {
 public:
  /// @param schema  Column descriptor (id, name, dtype).
  /// @param chunk_id  Numeric chunk ID (monotonically increasing per column).
  /// @param base_offset  The byte offset within the final file at which this
  ///                     chunk's data region starts.  All returned
  ///                     ColumnLocator::data_offset values are relative to the
  ///                     beginning of the file (base_offset + local_pos).
  ColumnChunkWriter(const ColumnSchema& schema,
                    uint32_t chunk_id,
                    uint64_t base_offset = 0);

  ~ColumnChunkWriter();

  // Non-copyable, movable.
  ColumnChunkWriter(const ColumnChunkWriter&) = delete;
  ColumnChunkWriter& operator=(const ColumnChunkWriter&) = delete;
  ColumnChunkWriter(ColumnChunkWriter&&) noexcept;
  ColumnChunkWriter& operator=(ColumnChunkWriter&&) noexcept;

  /// Append a single value.  Returns a ColumnLocator with the physical
  /// position where the value was written.
  StatusOr<ColumnLocator> Append(const void* data, size_t len);

  /// Convenience overload for Datum.
  StatusOr<ColumnLocator> Append(const Datum& datum);

  /// Number of values appended so far.
  uint32_t num_records() const;

  /// Current size of the data buffer (uncompressed bytes written).
  size_t data_size() const;

  /// Current size of the offset table (0 for fixed-width columns).
  size_t offset_table_size() const;

  /// Flush the chunk to a file descriptor at the configured base_offset.
  /// Writes: [data_region] [offset_table (if var-len)] [padding].
  /// Returns total bytes written.
  StatusOr<size_t> FlushTo(int fd) const;

  /// Serialise the chunk into a contiguous byte buffer (for testing or
  /// in-memory use).  Layout: [data_region][offset_table].
  StatusOr<std::vector<uint8_t>> Serialise() const;

  // --- Accessors for metadata generation --------------------------------

  uint32_t chunk_id() const;
  uint64_t base_offset() const;
  const ColumnSchema& schema() const;

  /// Raw pointer to the data buffer.
  const uint8_t* data_buffer() const;

  /// Raw pointer to the offset table (nullptr for fixed-width).
  const uint64_t* offset_table() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vdb
