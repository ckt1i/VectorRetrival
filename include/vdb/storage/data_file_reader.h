#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"

namespace vdb {
namespace storage {

// ============================================================================
// DataFileReader — reads row-major records from a .dat file via pread
// ============================================================================

/// Reads individual records from a DataFile by physical address.
///
/// Records are stored row-major:
///   [raw_vector (dim × 4 bytes) | payload_col0 | payload_col1 | ...]
///
/// This reader uses pread() for random access, which is thread-safe
/// (multiple threads can read from the same fd concurrently).
///
/// Usage:
///   DataFileReader reader;
///   reader.Open(path, dim, payload_columns);
///   auto [vec, payload] = reader.ReadRecord(address_entry);
///
class DataFileReader {
 public:
    DataFileReader();
    ~DataFileReader();

    VDB_DISALLOW_COPY(DataFileReader);

    // Move semantics (transfers fd ownership)
    DataFileReader(DataFileReader&& other) noexcept;
    DataFileReader& operator=(DataFileReader&& other) noexcept;

    /// Open a DataFile for reading.
    ///
    /// @param path             File path
    /// @param dim              Vector dimensionality (must match writer)
    /// @param payload_schemas  Payload column definitions (must match writer)
    /// @return Status
    Status Open(const std::string& path,
                Dim dim,
                const std::vector<ColumnSchema>& payload_schemas = {});

    /// Close the file.
    void Close();

    /// Read a complete record: raw vector + all payload columns.
    ///
    /// @param addr        Physical address from AddressColumn
    /// @param out_vec     Output vector buffer (caller pre-allocates dim floats)
    /// @param out_payload Output payload values (one per schema column)
    /// @return Status
    Status ReadRecord(const AddressEntry& addr,
                      float* out_vec,
                      std::vector<Datum>& out_payload) const;

    /// Read raw bytes at a given offset (for partial reads).
    ///
    /// This supports ReadTaskType::FRONT / VEC_ONLY / BACK splits where
    /// the caller controls exactly which byte range to read.
    ///
    /// @param offset      Byte offset in the file
    /// @param length      Number of bytes to read
    /// @param out_buffer  Output buffer (caller pre-allocates)
    /// @return Status
    Status ReadRaw(uint64_t offset, uint32_t length,
                   uint8_t* out_buffer) const;

    /// Read only the raw vector from a record.
    ///
    /// @param addr     Physical address
    /// @param out_vec  Output vector buffer (dim floats)
    /// @return Status
    Status ReadVector(const AddressEntry& addr, float* out_vec) const;

    /// Check if the reader is open.
    bool is_open() const { return fd_ >= 0; }

    /// Vector dimensionality.
    Dim dim() const { return dim_; }

    /// File path.
    const std::string& path() const { return path_; }

 private:
    int fd_ = -1;
    std::string path_;
    Dim dim_ = 0;
    std::vector<ColumnSchema> payload_schemas_;

    /// Parse payload columns from a raw buffer starting at `buf_offset`.
    Status ParsePayload(const uint8_t* buf, uint32_t buf_len,
                        uint32_t buf_offset,
                        std::vector<Datum>& out_payload) const;
};

}  // namespace storage
}  // namespace vdb
