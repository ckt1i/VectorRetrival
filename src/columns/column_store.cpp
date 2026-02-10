#include "vdb/columns/column_store.h"
#include "vdb/columns/column_chunk_writer.h"
#include "vdb/columns/column_chunk_reader.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace vdb {

// ============================================================================
// Impl
// ============================================================================

struct ColumnStore::Impl {
  // Schema -------------------------------------------------------------------
  std::vector<ColumnSchema> schemas;

  // Write path ---------------------------------------------------------------
  // One writer per column (single chunk per column in this MVP).
  std::vector<std::unique_ptr<ColumnChunkWriter>> writers;
  uint64_t num_rows = 0;
  bool     opened_for_write = false;

  // Read path ----------------------------------------------------------------
  // In-memory buffer that backs all readers.
  std::vector<uint8_t>                            read_buf;
  std::vector<std::unique_ptr<ColumnChunkReader>> readers;
  bool     opened_for_read = false;

  // Per-column region layout inside the serialised buffer:
  //   region_offsets[i] = start of column i's data in the buffer.
  std::vector<uint64_t> region_offsets;

  // Map column_id → index in schemas/writers/readers.
  std::map<ColumnID, size_t> id_to_idx;

  void BuildIdMap() {
    id_to_idx.clear();
    for (size_t i = 0; i < schemas.size(); ++i) {
      id_to_idx[schemas[i].id] = i;
    }
  }
};

// ============================================================================
// Ctor / dtor / move
// ============================================================================

ColumnStore::ColumnStore() : impl_(std::make_unique<Impl>()) {}
ColumnStore::~ColumnStore() = default;
ColumnStore::ColumnStore(ColumnStore&&) noexcept = default;
ColumnStore& ColumnStore::operator=(ColumnStore&&) noexcept = default;

// ============================================================================
// Open (write path)
// ============================================================================

Status ColumnStore::Open(const std::vector<ColumnSchema>& schemas) {
  if (schemas.empty()) {
    return Status::InvalidArgument("Open: schema list is empty");
  }

  auto& d = *impl_;
  d.schemas = schemas;
  d.BuildIdMap();
  d.num_rows = 0;
  d.opened_for_write = true;

  // Create one ColumnChunkWriter per column.
  // base_offset will be assigned later during Serialise/Flush; for now
  // we use 0 and adjust locators during serialisation.
  d.writers.clear();
  d.writers.reserve(schemas.size());
  for (size_t i = 0; i < schemas.size(); ++i) {
    d.writers.push_back(
        std::make_unique<ColumnChunkWriter>(schemas[i],
                                            /*chunk_id=*/0,
                                            /*base_offset=*/0));
  }

  return Status::OK();
}

// ============================================================================
// Ingest
// ============================================================================

StatusOr<IngestResult> ColumnStore::Ingest(
    const std::map<ColumnID, Datum>& columns) {
  auto& d = *impl_;
  if (!d.opened_for_write) {
    return Status::InvalidArgument("Ingest: store not opened for writing");
  }

  IngestResult result;
  result.column_locs.reserve(d.schemas.size());

  for (size_t i = 0; i < d.schemas.size(); ++i) {
    const auto& schema = d.schemas[i];
    auto it = columns.find(schema.id);
    if (it == columns.end()) {
      return Status::InvalidArgument(
          "Ingest: missing column " + std::to_string(schema.id));
    }

    auto loc_result = d.writers[i]->Append(it->second);
    if (!loc_result.ok()) {
      return loc_result.status();
    }
    result.column_locs.emplace_back(schema.id, *loc_result);
  }

  ++d.num_rows;
  return result;
}

uint64_t ColumnStore::num_rows() const { return impl_->num_rows; }

// ============================================================================
// SerialiseAll
// ============================================================================
// Layout:
//   [num_columns (uint32_t)]
//   [region_offset_0 (uint64_t)]
//   [region_offset_1 (uint64_t)]
//   ...
//   [col0_data][col0_offset_table][col1_data][col1_offset_table]...
//
// The header allows a fresh reader to reconstruct region_offsets.

StatusOr<std::vector<uint8_t>> ColumnStore::SerialiseAll() const {
  const auto& d = *impl_;
  if (!d.opened_for_write) {
    return Status::InvalidArgument("SerialiseAll: store not opened for writing");
  }

  const size_t num_cols = d.writers.size();

  // Header size: 4 bytes (num_columns) + 8 bytes per column (region_offset).
  const size_t header_size = sizeof(uint32_t) + num_cols * sizeof(uint64_t);

  // 1. Calculate data size and per-column region offsets (relative to data start).
  std::vector<uint64_t> offsets(num_cols);
  uint64_t cursor = 0;
  for (size_t i = 0; i < num_cols; ++i) {
    offsets[i] = cursor;
    cursor += d.writers[i]->data_size();
    cursor += d.writers[i]->offset_table_size();
  }

  // Total size = header + data.
  const size_t total_size = header_size + cursor;

  // 2. Allocate output buffer.
  std::vector<uint8_t> buf(total_size);
  uint8_t* ptr = buf.data();

  // 3. Write header.
  uint32_t nc = static_cast<uint32_t>(num_cols);
  std::memcpy(ptr, &nc, sizeof(nc));
  ptr += sizeof(nc);

  for (size_t i = 0; i < num_cols; ++i) {
    std::memcpy(ptr, &offsets[i], sizeof(uint64_t));
    ptr += sizeof(uint64_t);
  }

  // 4. Copy each column's data + offset table.
  for (size_t i = 0; i < num_cols; ++i) {
    const auto& w = *d.writers[i];

    // Data region.
    if (w.data_size() > 0) {
      std::memcpy(ptr, w.data_buffer(), w.data_size());
    }
    ptr += w.data_size();

    // Offset table (variable-length columns).
    if (w.offset_table_size() > 0) {
      std::memcpy(ptr, w.offset_table(), w.offset_table_size());
    }
    ptr += w.offset_table_size();
  }

  return buf;
}

// ============================================================================
// Flush (write to file)
// ============================================================================

StatusOr<size_t> ColumnStore::Flush(const std::string& path) const {
  auto buf_result = SerialiseAll();
  if (!buf_result.ok()) {
    return buf_result.status();
  }
  auto& buf = *buf_result;

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::IOError("Flush: failed to open " + path);
  }

  const uint8_t* ptr = buf.data();
  size_t remaining = buf.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, ptr, remaining);
    if (n < 0) {
      ::close(fd);
      return Status::IOError("Flush: write failed");
    }
    ptr       += n;
    remaining -= static_cast<size_t>(n);
  }

  ::close(fd);
  return buf.size();
}

// ============================================================================
// OpenForRead (in-memory)
// ============================================================================
// Buffer layout (with header):
//   [num_columns (uint32_t)]
//   [region_offset_0 (uint64_t)]
//   [region_offset_1 (uint64_t)]
//   ...
//   [col0_data][col0_offset_table][col1_data][col1_offset_table]...

Status ColumnStore::OpenForRead(const uint8_t* data, size_t size,
                                const std::vector<ColumnSchema>& schemas) {
  auto& d = *impl_;
  d.schemas = schemas;
  d.BuildIdMap();
  d.opened_for_read = true;

  // Copy data into our own buffer so we own the lifetime.
  d.read_buf.assign(data, data + size);

  const size_t num_cols = schemas.size();
  const size_t header_size = sizeof(uint32_t) + num_cols * sizeof(uint64_t);

  if (size < header_size) {
    return Status::InvalidArgument("OpenForRead: buffer too small for header");
  }

  // Parse header.
  const uint8_t* ptr = d.read_buf.data();

  uint32_t stored_num_cols = 0;
  std::memcpy(&stored_num_cols, ptr, sizeof(stored_num_cols));
  ptr += sizeof(stored_num_cols);

  if (stored_num_cols != num_cols) {
    return Status::InvalidArgument(
        "OpenForRead: schema mismatch (expected " + std::to_string(num_cols) +
        " columns, file has " + std::to_string(stored_num_cols) + ")");
  }

  d.region_offsets.resize(num_cols);
  for (size_t i = 0; i < num_cols; ++i) {
    std::memcpy(&d.region_offsets[i], ptr, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
  }

  // The data_base is where the column data starts (after header).
  const uint8_t* data_base = d.read_buf.data() + header_size;
  const size_t   data_size = size - header_size;

  d.readers.clear();
  d.readers.reserve(num_cols);
  for (size_t i = 0; i < num_cols; ++i) {
    // Each reader is given the data region starting after the header.
    // The region_offsets are relative to this data_base.
    d.readers.push_back(
        std::make_unique<ColumnChunkReader>(
            data_base, data_size,
            schemas[i], /*base_offset=*/0));
  }

  return Status::OK();
}

// ============================================================================
// OpenForRead (file)
// ============================================================================

Status ColumnStore::OpenForRead(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("OpenForRead: cannot open " + path);
  }

  struct stat st;
  if (::fstat(fd, &st) < 0) {
    ::close(fd);
    return Status::IOError("OpenForRead: fstat failed");
  }

  auto& d = *impl_;
  d.read_buf.resize(static_cast<size_t>(st.st_size));
  size_t remaining = d.read_buf.size();
  uint8_t* ptr = d.read_buf.data();
  while (remaining > 0) {
    ssize_t n = ::read(fd, ptr, remaining);
    if (n <= 0) {
      ::close(fd);
      return Status::IOError("OpenForRead: read failed");
    }
    ptr       += n;
    remaining -= static_cast<size_t>(n);
  }
  ::close(fd);

  // We need schemas to construct readers.  For the MVP, the caller must have
  // set them via Open() or we require the overload with schemas.  If schemas
  // are empty, return an error.
  if (d.schemas.empty()) {
    return Status::InvalidArgument(
        "OpenForRead(path): schemas not set.  Call Open() first or use "
        "the overload that accepts schemas.");
  }

  const size_t num_cols = d.schemas.size();
  const size_t header_size = sizeof(uint32_t) + num_cols * sizeof(uint64_t);
  const size_t file_size = d.read_buf.size();

  if (file_size < header_size) {
    return Status::InvalidArgument("OpenForRead: file too small for header");
  }

  // Parse header.
  const uint8_t* hptr = d.read_buf.data();

  uint32_t stored_num_cols = 0;
  std::memcpy(&stored_num_cols, hptr, sizeof(stored_num_cols));
  hptr += sizeof(stored_num_cols);

  if (stored_num_cols != num_cols) {
    return Status::InvalidArgument(
        "OpenForRead: schema mismatch (expected " + std::to_string(num_cols) +
        " columns, file has " + std::to_string(stored_num_cols) + ")");
  }

  d.region_offsets.resize(num_cols);
  for (size_t i = 0; i < num_cols; ++i) {
    std::memcpy(&d.region_offsets[i], hptr, sizeof(uint64_t));
    hptr += sizeof(uint64_t);
  }

  d.BuildIdMap();
  d.opened_for_read = true;

  // The data region starts after the header.
  const uint8_t* data_base = d.read_buf.data() + header_size;
  const size_t   data_size = file_size - header_size;

  d.readers.clear();
  d.readers.reserve(num_cols);
  for (size_t i = 0; i < num_cols; ++i) {
    d.readers.push_back(
        std::make_unique<ColumnChunkReader>(
            data_base, data_size,
            d.schemas[i], /*base_offset=*/0));
  }

  return Status::OK();
}

// ============================================================================
// ReadColumn
// ============================================================================

StatusOr<Datum> ColumnStore::ReadColumn(ColumnID col_id,
                                        const ColumnLocator& loc) const {
  const auto& d = *impl_;
  if (!d.opened_for_read) {
    return Status::InvalidArgument("ReadColumn: store not opened for reading");
  }
  auto it = d.id_to_idx.find(col_id);
  if (it == d.id_to_idx.end()) {
    return Status::NotFound("ReadColumn: unknown column " +
                            std::to_string(col_id));
  }
  size_t idx = it->second;

  // Adjust the locator's data_offset and offset_table_pos by the region offset.
  // The writer returned locators relative to its local buffer (base_offset=0).
  // In the serialized buffer, each column starts at region_offsets[idx].
  ColumnLocator adjusted = loc;
  if (idx < d.region_offsets.size()) {
    adjusted.data_offset += d.region_offsets[idx];
    adjusted.offset_table_pos += d.region_offsets[idx];
  }

  return d.readers[idx]->ReadDatum(adjusted);
}

// ============================================================================
// Read (all columns)
// ============================================================================

StatusOr<std::map<ColumnID, Datum>> ColumnStore::Read(
    const std::vector<std::pair<ColumnID, ColumnLocator>>& locs) const {
  std::map<ColumnID, Datum> row;
  for (const auto& [col_id, loc] : locs) {
    auto datum_result = ReadColumn(col_id, loc);
    if (!datum_result.ok()) {
      return datum_result.status();
    }
    row.emplace(col_id, std::move(*datum_result));
  }
  return row;
}

// ============================================================================
// ReadColumns (projection push-down)
// ============================================================================

StatusOr<std::map<ColumnID, Datum>> ColumnStore::ReadColumns(
    const std::vector<std::pair<ColumnID, ColumnLocator>>& locs,
    const std::vector<ColumnID>& column_ids) const {
  // Build fast-lookup set for requested columns.
  std::map<ColumnID, const ColumnLocator*> wanted;
  for (const auto& cid : column_ids) {
    wanted[cid] = nullptr;
  }
  for (const auto& [col_id, loc] : locs) {
    auto it = wanted.find(col_id);
    if (it != wanted.end()) {
      it->second = &loc;
    }
  }

  std::map<ColumnID, Datum> row;
  for (const auto& [cid, loc_ptr] : wanted) {
    if (loc_ptr == nullptr) {
      return Status::NotFound("ReadColumns: locator not found for column " +
                              std::to_string(cid));
    }
    auto datum_result = ReadColumn(cid, *loc_ptr);
    if (!datum_result.ok()) {
      return datum_result.status();
    }
    row.emplace(cid, std::move(*datum_result));
  }
  return row;
}

// ============================================================================
// Schema introspection
// ============================================================================

const std::vector<ColumnSchema>& ColumnStore::schemas() const {
  return impl_->schemas;
}

}  // namespace vdb
