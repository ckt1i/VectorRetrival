#include "vdb/columns/column_chunk_writer.h"

#include <cstring>
#include <unistd.h>

namespace vdb {

// ============================================================================
// Impl
// ============================================================================

struct ColumnChunkWriter::Impl {
  ColumnSchema schema;
  uint32_t     chunk_id;
  uint64_t     base_offset;   // file-level offset where this chunk starts

  // Data region ---------------------------------------------------------------
  std::vector<uint8_t> data_buf;

  // Offset table (variable-length columns only) ------------------------------
  //   offset_table[i] = byte offset within data_buf where value i starts.
  //   offset_table[num_records] = end sentinel.
  std::vector<uint64_t> offsets;

  uint32_t num_records = 0;
  bool     is_fixed_width = false;
  size_t   fixed_width    = 0;

  Impl(const ColumnSchema& s, uint32_t cid, uint64_t base)
      : schema(s), chunk_id(cid), base_offset(base) {
    is_fixed_width = DTypeIsFixedWidth(s.dtype);
    fixed_width    = DTypeSize(s.dtype);

    // Reserve some space up-front.
    data_buf.reserve(4096);

    if (!is_fixed_width) {
      // First entry: offset 0 (start of first value).
      offsets.reserve(256);
      offsets.push_back(0);
    }
  }
};

// ============================================================================
// Ctor / dtor / move
// ============================================================================

ColumnChunkWriter::ColumnChunkWriter(const ColumnSchema& schema,
                                     uint32_t chunk_id,
                                     uint64_t base_offset)
    : impl_(std::make_unique<Impl>(schema, chunk_id, base_offset)) {}

ColumnChunkWriter::~ColumnChunkWriter() = default;

ColumnChunkWriter::ColumnChunkWriter(ColumnChunkWriter&&) noexcept = default;
ColumnChunkWriter& ColumnChunkWriter::operator=(ColumnChunkWriter&&) noexcept = default;

// ============================================================================
// Append
// ============================================================================

StatusOr<ColumnLocator> ColumnChunkWriter::Append(const void* data,
                                                  size_t len) {
  if (data == nullptr && len > 0) {
    return Status::InvalidArgument("Append: null data pointer with non-zero length");
  }

  auto& d = *impl_;

  if (d.is_fixed_width) {
    // --- Fixed-width path ---------------------------------------------------
    if (len != d.fixed_width) {
      return Status::InvalidArgument(
          "Append: expected " + std::to_string(d.fixed_width) +
          " bytes for fixed-width column, got " + std::to_string(len));
    }

    // Physical position within the file.
    uint64_t file_offset = d.base_offset + d.data_buf.size();

    // Copy value into data_buf.
    const auto* src = static_cast<const uint8_t*>(data);
    d.data_buf.insert(d.data_buf.end(), src, src + len);

    ColumnLocator loc;
    loc.chunk_id         = d.chunk_id;
    loc.data_offset      = file_offset;
    loc.data_length      = static_cast<uint32_t>(len);
    loc.offset_table_pos = 0;  // unused for fixed-width

    ++d.num_records;
    return loc;
  }

  // --- Variable-length path -------------------------------------------------
  uint64_t local_pos   = d.data_buf.size();
  uint64_t file_offset = d.base_offset + local_pos;

  // Append bytes.
  if (len > 0) {
    const auto* src = static_cast<const uint8_t*>(data);
    d.data_buf.insert(d.data_buf.end(), src, src + len);
  }

  uint32_t slot = d.num_records;

  // Record end-offset for the next value.
  d.offsets.push_back(d.data_buf.size());

  ColumnLocator loc;
  loc.chunk_id         = d.chunk_id;
  loc.data_offset      = file_offset;
  loc.data_length      = static_cast<uint32_t>(len);
  loc.offset_table_pos = slot;

  ++d.num_records;
  return loc;
}

StatusOr<ColumnLocator> ColumnChunkWriter::Append(const Datum& datum) {
  if (DTypeIsFixedWidth(datum.dtype)) {
    return Append(datum.fixed_data(), datum.byte_size());
  }
  return Append(datum.var_data.data(), datum.var_data.size());
}

// ============================================================================
// Queries
// ============================================================================

uint32_t ColumnChunkWriter::num_records() const { return impl_->num_records; }

size_t ColumnChunkWriter::data_size() const { return impl_->data_buf.size(); }

size_t ColumnChunkWriter::offset_table_size() const {
  if (impl_->is_fixed_width) return 0;
  return impl_->offsets.size() * sizeof(uint64_t);
}

uint32_t ColumnChunkWriter::chunk_id() const { return impl_->chunk_id; }
uint64_t ColumnChunkWriter::base_offset() const { return impl_->base_offset; }
const ColumnSchema& ColumnChunkWriter::schema() const { return impl_->schema; }

const uint8_t* ColumnChunkWriter::data_buffer() const {
  return impl_->data_buf.data();
}

const uint64_t* ColumnChunkWriter::offset_table() const {
  if (impl_->is_fixed_width) return nullptr;
  return impl_->offsets.data();
}

// ============================================================================
// Serialise  (data + offset_table → contiguous bytes)
// ============================================================================

StatusOr<std::vector<uint8_t>> ColumnChunkWriter::Serialise() const {
  const auto& d = *impl_;

  size_t total = d.data_buf.size();
  if (!d.is_fixed_width) {
    total += d.offsets.size() * sizeof(uint64_t);
  }

  std::vector<uint8_t> out(total);
  std::memcpy(out.data(), d.data_buf.data(), d.data_buf.size());

  if (!d.is_fixed_width) {
    std::memcpy(out.data() + d.data_buf.size(),
                d.offsets.data(),
                d.offsets.size() * sizeof(uint64_t));
  }

  return out;
}

// ============================================================================
// FlushTo  (write to file descriptor at base_offset)
// ============================================================================

StatusOr<size_t> ColumnChunkWriter::FlushTo(int fd) const {
  const auto& d = *impl_;

  // Write data region.
  {
    const uint8_t* ptr = d.data_buf.data();
    size_t remaining = d.data_buf.size();
    off_t  pos       = static_cast<off_t>(d.base_offset);

    while (remaining > 0) {
      ssize_t n = ::pwrite(fd, ptr, remaining, pos);
      if (n < 0) {
        return Status::IOError("pwrite data region failed");
      }
      ptr       += n;
      pos       += n;
      remaining -= static_cast<size_t>(n);
    }
  }

  size_t bytes_written = d.data_buf.size();

  // Write offset table (variable-length columns).
  if (!d.is_fixed_width) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(d.offsets.data());
    size_t remaining = d.offsets.size() * sizeof(uint64_t);
    off_t  pos       = static_cast<off_t>(d.base_offset + d.data_buf.size());

    while (remaining > 0) {
      ssize_t n = ::pwrite(fd, ptr, remaining, pos);
      if (n < 0) {
        return Status::IOError("pwrite offset table failed");
      }
      ptr       += n;
      pos       += n;
      remaining -= static_cast<size_t>(n);
    }
    bytes_written += d.offsets.size() * sizeof(uint64_t);
  }

  return bytes_written;
}

}  // namespace vdb
