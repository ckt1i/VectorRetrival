#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/common/status.h"
#include "vdb/common/aligned_alloc.h"

namespace vdb {

// ============================================================================
// IngestResult — returned by ColumnStore::Ingest()
// ============================================================================

struct IngestResult {
  std::vector<std::pair<ColumnID, ColumnLocator>> column_locs;
};

// ============================================================================
// ColumnStore
// ============================================================================
// Manages multiple columns: writing, reading, and serialising to a `.cols`
// file.  Every record is addressed exclusively by physical offsets
// (ColumnLocator) — there is no row-id → offset translation.
//
// Typical lifecycle:
//   1. Open() with a schema (set of ColumnSchema).
//   2. Ingest() rows one-by-one; each call returns an IngestResult with
//      per-column physical locators.
//   3. Flush() serialises everything into a single `.cols` file.
//   4. For reading, OpenForRead() loads the file, and Read() / ReadColumn()
//      retrieve values by ColumnLocator.
// ============================================================================

class ColumnStore {
 public:
  ColumnStore();
  ~ColumnStore();

  // Non-copyable, movable.
  ColumnStore(const ColumnStore&) = delete;
  ColumnStore& operator=(const ColumnStore&) = delete;
  ColumnStore(ColumnStore&&) noexcept;
  ColumnStore& operator=(ColumnStore&&) noexcept;

  // ---- Write path ----------------------------------------------------------

  /// Initialise the store for writing with the given column schemas.
  Status Open(const std::vector<ColumnSchema>& schemas);

  /// Append one row.  `columns` maps ColumnID → Datum.
  /// Returns per-column physical locators.
  StatusOr<IngestResult> Ingest(
      const std::map<ColumnID, Datum>& columns);

  /// Number of rows ingested so far.
  uint64_t num_rows() const;

  /// Flush all column data to a `.cols` file at `path`.
  /// Returns total bytes written.
  StatusOr<size_t> Flush(const std::string& path) const;

  /// Serialise all column data into a single in-memory buffer.
  /// Layout:  [col0_data][col0_offsets][col1_data][col1_offsets]...
  /// Returns the buffer.
  StatusOr<std::vector<uint8_t>> SerialiseAll() const;

  // ---- Read path -----------------------------------------------------------

  /// Open an existing `.cols` file (or in-memory buffer) for reading.
  /// After this call, Read() / ReadColumn() can be used.
  Status OpenForRead(const std::string& path);

  /// Open from an in-memory buffer (e.g. returned by SerialiseAll).
  Status OpenForRead(const uint8_t* data, size_t size,
                     const std::vector<ColumnSchema>& schemas);

  /// Read a single column value by physical locator.
  StatusOr<Datum> ReadColumn(ColumnID col_id,
                             const ColumnLocator& loc) const;

  /// Read all columns for one record.
  StatusOr<std::map<ColumnID, Datum>> Read(
      const std::vector<std::pair<ColumnID, ColumnLocator>>& locs) const;

  /// Read a subset of columns for one record (projection push-down).
  StatusOr<std::map<ColumnID, Datum>> ReadColumns(
      const std::vector<std::pair<ColumnID, ColumnLocator>>& locs,
      const std::vector<ColumnID>& column_ids) const;

  // ---- Schema introspection ------------------------------------------------
  const std::vector<ColumnSchema>& schemas() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vdb
