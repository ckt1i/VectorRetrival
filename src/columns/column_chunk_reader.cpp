#include "vdb/columns/column_chunk_reader.h"

#include <cstring>
#include <unistd.h>

namespace vdb {

// ============================================================================
// Impl
// ============================================================================

struct ColumnChunkReader::Impl {
  // Source -------------------------------------------------------------------
  const uint8_t* mem_data  = nullptr;   // memory-backed
  size_t         mem_size  = 0;
  int            fd        = -1;        // file-backed (-1 → memory mode)

  ColumnSchema   schema;
  uint64_t       base_offset = 0;
  bool           is_fixed_width = false;
  size_t         fixed_width    = 0;

  Impl(const ColumnSchema& s, uint64_t base)
      : schema(s), base_offset(base) {
    is_fixed_width = DTypeIsFixedWidth(s.dtype);
    fixed_width    = DTypeSize(s.dtype);
  }

  // ----- helpers ------------------------------------------------------------

  /// Read `len` bytes starting at file/buffer offset `abs_offset`.
  Status ReadBytes(uint64_t abs_offset, size_t len, void* dst) const {
    if (mem_data) {
      // Memory-backed read.
      uint64_t local = abs_offset - base_offset;
      if (local + len > mem_size) {
        return Status::IOError("ReadBytes: out of bounds (local=" +
                               std::to_string(local) + " len=" +
                               std::to_string(len) + " size=" +
                               std::to_string(mem_size) + ")");
      }
      std::memcpy(dst, mem_data + local, len);
      return Status::OK();
    }

    // File-backed read via pread().
    auto* ptr = static_cast<uint8_t*>(dst);
    size_t remaining = len;
    off_t  pos       = static_cast<off_t>(abs_offset);
    while (remaining > 0) {
      ssize_t n = ::pread(fd, ptr, remaining, pos);
      if (n <= 0) {
        return Status::IOError("pread failed");
      }
      ptr       += n;
      pos       += n;
      remaining -= static_cast<size_t>(n);
    }
    return Status::OK();
  }
};

// ============================================================================
// Ctor / dtor / move
// ============================================================================

ColumnChunkReader::ColumnChunkReader(const uint8_t* data,
                                     size_t data_size,
                                     const ColumnSchema& schema,
                                     uint64_t base_offset)
    : impl_(std::make_unique<Impl>(schema, base_offset)) {
  impl_->mem_data = data;
  impl_->mem_size = data_size;
}

ColumnChunkReader::ColumnChunkReader(int fd,
                                     const ColumnSchema& schema,
                                     uint64_t base_offset)
    : impl_(std::make_unique<Impl>(schema, base_offset)) {
  impl_->fd = fd;
}

ColumnChunkReader::~ColumnChunkReader() = default;
ColumnChunkReader::ColumnChunkReader(ColumnChunkReader&&) noexcept = default;
ColumnChunkReader& ColumnChunkReader::operator=(ColumnChunkReader&&) noexcept = default;

// ============================================================================
// ReadAt
// ============================================================================

StatusOr<AlignedBuffer> ColumnChunkReader::ReadAt(const ColumnLocator& loc) const {
  const auto& d = *impl_;

  if (loc.data_length == 0) {
    return AlignedBuffer();  // empty value (e.g. empty string)
  }

  AlignedBuffer buf(loc.data_length);
  VDB_RETURN_IF_ERROR(d.ReadBytes(loc.data_offset, loc.data_length, buf.data()));
  return buf;
}

// ============================================================================
// ReadBatch
// ============================================================================

Status ColumnChunkReader::ReadBatch(const std::vector<ColumnLocator>& locs,
                                    std::vector<AlignedBuffer>* out) const {
  if (out == nullptr) {
    return Status::InvalidArgument("ReadBatch: out is null");
  }
  out->clear();
  out->reserve(locs.size());

  for (const auto& loc : locs) {
    auto result = ReadAt(loc);
    if (!result.ok()) {
      return result.status();
    }
    out->push_back(std::move(*result));
  }
  return Status::OK();
}

// ============================================================================
// ReadDatum — reconstruct a Datum from the raw bytes
// ============================================================================

StatusOr<Datum> ColumnChunkReader::ReadDatum(const ColumnLocator& loc) const {
  const auto& d = *impl_;

  if (d.is_fixed_width) {
    // Fixed-width: read directly into Datum.
    Datum datum;
    datum.dtype = d.schema.dtype;
    datum.fixed = {};

    if (loc.data_length > sizeof(datum.fixed)) {
      return Status::Internal("ReadDatum: fixed value too large");
    }

    VDB_RETURN_IF_ERROR(
        d.ReadBytes(loc.data_offset, loc.data_length, &datum.fixed));
    return datum;
  }

  // Variable-length: read into var_data.
  Datum datum;
  datum.dtype = d.schema.dtype;

  if (loc.data_length == 0) {
    return datum;  // empty string / bytes
  }

  datum.var_data.resize(loc.data_length);
  VDB_RETURN_IF_ERROR(
      d.ReadBytes(loc.data_offset, loc.data_length,
                  datum.var_data.data()));
  return datum;
}

}  // namespace vdb
