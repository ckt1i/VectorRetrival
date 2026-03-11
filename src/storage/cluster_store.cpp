#include "vdb/storage/cluster_store.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "vdb/simd/popcount.h"

namespace vdb {
namespace storage {

/// Magic number for ClusterStore trailer: "VCLU" (0x554C4356 little-endian)
static constexpr uint32_t kCluTrailerMagic = 0x56434C55;

// ============================================================================
// ClusterStoreWriter
// ============================================================================

ClusterStoreWriter::ClusterStoreWriter() = default;

ClusterStoreWriter::~ClusterStoreWriter() {
    if (file_.is_open() && !finalized_) {
        file_.close();
    }
}

Status ClusterStoreWriter::Open(const std::string& path,
                                 uint32_t cluster_id,
                                 Dim dim,
                                 const RaBitQConfig& rabitq_config) {
    if (file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter already open");
    }

    path_ = path;
    info_.cluster_id = cluster_id;
    info_.dim = dim;
    info_.rabitq_config = rabitq_config;
    info_.num_records = 0;
    current_offset_ = 0;
    centroid_written_ = false;
    vectors_written_ = false;
    address_written_ = false;
    finalized_ = false;

    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    return Status::OK();
}

// ============================================================================
// WriteCentroid
// ============================================================================

Status ClusterStoreWriter::WriteCentroid(const float* centroid) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (centroid_written_) {
        return Status::InvalidArgument("Centroid already written");
    }

    info_.centroid_offset = current_offset_;
    const uint32_t bytes = info_.dim * sizeof(float);

    file_.write(reinterpret_cast<const char*>(centroid), bytes);
    if (!file_.good()) {
        return Status::IOError("Failed to write centroid");
    }
    current_offset_ += bytes;
    info_.centroid_length = bytes;
    centroid_written_ = true;

    return Status::OK();
}

// ============================================================================
// WriteVectors — write RaBitQ codes + norms
// ============================================================================

Status ClusterStoreWriter::WriteVectors(
    const std::vector<rabitq::RaBitQCode>& codes) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (!centroid_written_) {
        return Status::InvalidArgument("Must write centroid before vectors");
    }
    if (vectors_written_) {
        return Status::InvalidArgument("Vectors already written");
    }

    const uint32_t N = static_cast<uint32_t>(codes.size());
    info_.num_records = N;

    // --- Write RaBitQ codes ---
    info_.rabitq_data_offset = current_offset_;

    for (uint32_t i = 0; i < N; ++i) {
        // Write code words
        const auto& code = codes[i].code;
        const uint32_t code_bytes =
            static_cast<uint32_t>(code.size()) * sizeof(uint64_t);
        file_.write(reinterpret_cast<const char*>(code.data()), code_bytes);
        if (!file_.good()) {
            return Status::IOError("Failed to write RaBitQ code");
        }
        current_offset_ += code_bytes;
    }

    info_.rabitq_data_length =
        static_cast<uint32_t>(current_offset_ - info_.rabitq_data_offset);

    vectors_written_ = true;
    return Status::OK();
}

// ============================================================================
// WriteAddressBlocks
// ============================================================================

Status ClusterStoreWriter::WriteAddressBlocks(
    const std::vector<AddressBlock>& blocks) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (!vectors_written_) {
        return Status::InvalidArgument(
            "Must write vectors before address blocks");
    }
    if (address_written_) {
        return Status::InvalidArgument("Address blocks already written");
    }

    info_.address_blocks_offset = current_offset_;

    // Save blocks metadata and write packed data
    info_.address_blocks = blocks;

    for (auto& block : info_.address_blocks) {
        // Write the packed bytes to the .clu file; record the offset
        // (The AddressBlock struct in info_ will be updated with position info
        //  for the reader to locate the data.)
        file_.write(reinterpret_cast<const char*>(block.packed.data()),
                    block.packed.size());
        if (!file_.good()) {
            return Status::IOError("Failed to write address block data");
        }
        current_offset_ += block.packed.size();
    }

    address_written_ = true;
    return Status::OK();
}

// ============================================================================
// Finalize — write trailer and close
// ============================================================================

namespace {

/// Helper: append raw bytes to a buffer.
inline void AppendBytes(std::vector<uint8_t>& buf, const void* data,
                        size_t len) {
    const auto* p = reinterpret_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}

template <typename T>
inline void AppendValue(std::vector<uint8_t>& buf, T value) {
    AppendBytes(buf, &value, sizeof(T));
}

/// Build ClusterInfo trailer as a byte buffer.
/// Layout (all little-endian, native on x86):
///   cluster_id       : u32
///   num_records      : u32
///   dim              : u32
///   rabitq_bits      : u8
///   rabitq_block_size: u32
///   rabitq_c_factor  : f32
///   centroid_offset  : u64
///   centroid_length  : u32
///   rabitq_data_offset: u64
///   rabitq_data_length: u32
///   data_file_path_len: u32
///   data_file_path   : char[data_file_path_len]
///   address_blocks_offset: u64
///   num_address_blocks: u32
///   For each address block:
///     base_offset    : u64
///     bit_width      : u8
///     record_count   : u32
///     page_size      : u32
///     packed_size    : u32
std::vector<uint8_t> BuildTrailer(
    const ClusterStoreWriter::ClusterInfo& info) {
    std::vector<uint8_t> buf;
    buf.reserve(256);

    AppendValue(buf, info.cluster_id);
    AppendValue(buf, info.num_records);
    AppendValue(buf, info.dim);
    AppendValue(buf, info.rabitq_config.bits);
    AppendValue(buf, info.rabitq_config.block_size);
    AppendValue(buf, info.rabitq_config.c_factor);
    AppendValue(buf, info.centroid_offset);
    AppendValue(buf, info.centroid_length);
    AppendValue(buf, info.rabitq_data_offset);
    AppendValue(buf, info.rabitq_data_length);

    const auto path_len =
        static_cast<uint32_t>(info.data_file_path.size());
    AppendValue(buf, path_len);
    AppendBytes(buf, info.data_file_path.data(), path_len);

    const auto num_blocks =
        static_cast<uint32_t>(info.address_blocks.size());
    AppendValue(buf, info.address_blocks_offset);
    AppendValue(buf, num_blocks);

    for (const auto& block : info.address_blocks) {
        AppendValue(buf, block.base_offset);
        AppendValue(buf, block.bit_width);
        AppendValue(buf, block.record_count);
        AppendValue(buf, block.page_size);
        const auto packed_size =
            static_cast<uint32_t>(block.packed.size());
        AppendValue(buf, packed_size);
    }

    return buf;
}

}  // anonymous namespace

Status ClusterStoreWriter::Finalize(const std::string& data_file_path) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("Already finalized");
    }

    info_.data_file_path = data_file_path;

    // --- Write trailer ---
    auto trailer = BuildTrailer(info_);
    file_.write(reinterpret_cast<const char*>(trailer.data()),
                static_cast<std::streamsize>(trailer.size()));
    if (!file_.good()) {
        return Status::IOError("Failed to write trailer");
    }

    // Write trailer_size (u32) + magic (u32) as the last 8 bytes
    const auto trailer_size = static_cast<uint32_t>(trailer.size());
    file_.write(reinterpret_cast<const char*>(&trailer_size),
                sizeof(uint32_t));
    file_.write(reinterpret_cast<const char*>(&kCluTrailerMagic),
                sizeof(uint32_t));

    file_.flush();
    file_.close();
    finalized_ = true;

    return Status::OK();
}

// ============================================================================
// ClusterStoreReader::ReadInfo — reconstruct ClusterInfo from .clu trailer
// ============================================================================

namespace {

/// Helper: read a value from a buffer at a given position, advance pos.
template <typename T>
inline bool ReadValue(const std::vector<uint8_t>& buf, size_t& pos, T& out) {
    if (pos + sizeof(T) > buf.size()) return false;
    std::memcpy(&out, buf.data() + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

inline bool ReadBytes(const std::vector<uint8_t>& buf, size_t& pos,
                      void* out, size_t len) {
    if (pos + len > buf.size()) return false;
    std::memcpy(out, buf.data() + pos, len);
    pos += len;
    return true;
}

}  // anonymous namespace

Status ClusterStoreReader::ReadInfo(
    const std::string& path,
    ClusterStoreWriter::ClusterInfo* out) {
    if (!out) {
        return Status::InvalidArgument("out must not be null");
    }

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    // Get file size
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return Status::IOError("Failed to stat file: " + path);
    }
    const auto file_size = static_cast<uint64_t>(st.st_size);
    if (file_size < 8) {
        ::close(fd);
        return Status::Corruption("File too small for trailer: " + path);
    }

    // Read last 8 bytes: trailer_size (u32) + magic (u32)
    uint32_t footer[2];
    ssize_t n = ::pread(fd, footer, 8,
                        static_cast<off_t>(file_size - 8));
    if (n != 8) {
        ::close(fd);
        return Status::IOError("Failed to read trailer footer");
    }

    const uint32_t trailer_size = footer[0];
    const uint32_t magic = footer[1];

    if (magic != kCluTrailerMagic) {
        ::close(fd);
        return Status::Corruption(
            "Invalid ClusterStore magic (expected 0x56434C55, got 0x" +
            std::to_string(magic) + ")");
    }

    if (file_size < trailer_size + 8) {
        ::close(fd);
        return Status::Corruption("Trailer size exceeds file size");
    }

    // Read trailer data
    std::vector<uint8_t> buf(trailer_size);
    n = ::pread(fd, buf.data(), trailer_size,
                static_cast<off_t>(file_size - 8 - trailer_size));
    ::close(fd);
    if (n != static_cast<ssize_t>(trailer_size)) {
        return Status::IOError("Failed to read trailer data");
    }

    // Parse trailer
    size_t pos = 0;
    auto& info = *out;

    if (!ReadValue(buf, pos, info.cluster_id) ||
        !ReadValue(buf, pos, info.num_records) ||
        !ReadValue(buf, pos, info.dim) ||
        !ReadValue(buf, pos, info.rabitq_config.bits) ||
        !ReadValue(buf, pos, info.rabitq_config.block_size) ||
        !ReadValue(buf, pos, info.rabitq_config.c_factor) ||
        !ReadValue(buf, pos, info.centroid_offset) ||
        !ReadValue(buf, pos, info.centroid_length) ||
        !ReadValue(buf, pos, info.rabitq_data_offset) ||
        !ReadValue(buf, pos, info.rabitq_data_length)) {
        return Status::Corruption("Trailer: failed to parse fixed fields");
    }

    uint32_t path_len = 0;
    if (!ReadValue(buf, pos, path_len)) {
        return Status::Corruption("Trailer: failed to parse path length");
    }
    if (pos + path_len > buf.size()) {
        return Status::Corruption("Trailer: path extends beyond trailer");
    }
    info.data_file_path.resize(path_len);
    if (path_len > 0) {
        if (!ReadBytes(buf, pos, info.data_file_path.data(), path_len)) {
            return Status::Corruption("Trailer: failed to read path");
        }
    }

    uint32_t num_blocks = 0;
    if (!ReadValue(buf, pos, info.address_blocks_offset) ||
        !ReadValue(buf, pos, num_blocks)) {
        return Status::Corruption("Trailer: failed to parse num_blocks");
    }

    info.address_blocks.resize(num_blocks);
    for (uint32_t i = 0; i < num_blocks; ++i) {
        auto& block = info.address_blocks[i];
        uint32_t packed_size = 0;

        if (!ReadValue(buf, pos, block.base_offset) ||
            !ReadValue(buf, pos, block.bit_width) ||
            !ReadValue(buf, pos, block.record_count) ||
            !ReadValue(buf, pos, block.page_size) ||
            !ReadValue(buf, pos, packed_size)) {
            return Status::Corruption("Trailer: failed to parse block " +
                                      std::to_string(i));
        }

        block.packed.resize(packed_size);  // allocate space; LoadAddressBlocks fills it
    }

    if (pos != buf.size()) {
        return Status::Corruption(
            "Trailer: extra bytes at end (parsed " +
            std::to_string(pos) + " of " +
            std::to_string(buf.size()) + ")");
    }

    return Status::OK();
}

// ============================================================================
// ClusterStoreReader
// ============================================================================

ClusterStoreReader::ClusterStoreReader() = default;

ClusterStoreReader::~ClusterStoreReader() {
    Close();
}

ClusterStoreReader::ClusterStoreReader(ClusterStoreReader&& other) noexcept
    : fd_(other.fd_), info_(std::move(other.info_)) {
    other.fd_ = -1;
}

ClusterStoreReader& ClusterStoreReader::operator=(
    ClusterStoreReader&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        info_ = std::move(other.info_);
        other.fd_ = -1;
    }
    return *this;
}

Status ClusterStoreReader::Open(
    const std::string& path,
    const ClusterStoreWriter::ClusterInfo& info) {
    if (fd_ >= 0) {
        return Status::InvalidArgument("ClusterStoreReader already open");
    }

    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    info_ = info;
    VDB_RETURN_IF_ERROR(LoadAddressBlocks());
    return Status::OK();
}

void ClusterStoreReader::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ============================================================================
// LoadCentroid
// ============================================================================

Status ClusterStoreReader::LoadCentroid(std::vector<float>& out) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    out.resize(info_.dim);
    const uint32_t bytes = info_.dim * sizeof(float);

    ssize_t n = ::pread(fd_, out.data(), bytes,
                        static_cast<off_t>(info_.centroid_offset));
    if (n != static_cast<ssize_t>(bytes)) {
        return Status::IOError("Failed to read centroid");
    }

    return Status::OK();
}

// ============================================================================
// LoadCode — single code
// ============================================================================

Status ClusterStoreReader::LoadCode(uint32_t record_idx,
                                     std::vector<uint64_t>& out_code) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }
    if (record_idx >= info_.num_records) {
        return Status::InvalidArgument("Record index out of range");
    }

    const uint32_t nwords = num_code_words();
    const uint32_t entry_sz = code_entry_size();

    // Seek to the code entry
    uint64_t code_offset = info_.rabitq_data_offset +
                           static_cast<uint64_t>(record_idx) * entry_sz;

    // Read code words
    out_code.resize(nwords);
    ssize_t n = ::pread(fd_, out_code.data(), nwords * sizeof(uint64_t),
                        static_cast<off_t>(code_offset));
    if (n != static_cast<ssize_t>(nwords * sizeof(uint64_t))) {
        return Status::IOError("Failed to read RaBitQ code");
    }

    return Status::OK();
}

// ============================================================================
// LoadCodes — batch
// ============================================================================

Status ClusterStoreReader::LoadCodes(
    const std::vector<uint32_t>& indices,
    std::vector<rabitq::RaBitQCode>& out_codes) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    out_codes.resize(indices.size());

    for (size_t i = 0; i < indices.size(); ++i) {
        std::vector<uint64_t> code;
        VDB_RETURN_IF_ERROR(LoadCode(indices[i], code));

        out_codes[i].code = std::move(code);
        out_codes[i].norm = 0.0f;
        out_codes[i].sum_x = simd::PopcountTotal(
            out_codes[i].code.data(),
            static_cast<uint32_t>(out_codes[i].code.size()));
    }

    return Status::OK();
}

// ============================================================================
// LoadAddressBlocks — read and decode all address blocks
// ============================================================================
Status ClusterStoreReader::LoadAddressBlocks() {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    //Phase1: Load all the packed data for address blocks
    uint64_t current_offset = info_.address_blocks_offset;

    for (auto& block : info_.address_blocks) {
        uint32_t packed_size = block.packed.size();  // 已知大小
        if (packed_size == 0) continue;

        block.packed.resize(packed_size);
        ssize_t n = ::pread(fd_, block.packed.data(), packed_size,
                            static_cast<off_t>(current_offset));
        if (n != static_cast<ssize_t>(packed_size)) {
            return Status::IOError("Failed to read address block data");
        }
        current_offset += packed_size;
    }

    //Phase2: Decode all blocks into decoded_addresses with SIMD
    info_.decoded_addresses.clear();
    info_.decoded_addresses.reserve(info_.num_records);

    for (const auto& block : info_.address_blocks) {
        std::vector<AddressEntry> block_entries;
        AddressColumn::DecodeBlock(block, block_entries);

        info_.decoded_addresses.insert(
            info_.decoded_addresses.end(),
            block_entries.begin(), 
            block_entries.end()
        );
    }

    return Status::OK();
}

// ============================================================================
// GetAddress / GetAddresses
// ============================================================================

AddressEntry ClusterStoreReader::GetAddress(uint32_t record_idx) const {
    if (record_idx >= info_.decoded_addresses.size()) {
        return AddressEntry{0, 0};  // 错误情况
    }
    return info_.decoded_addresses[record_idx];
}

std::vector<AddressEntry> ClusterStoreReader::GetAddresses(
    const std::vector<uint32_t>& indices) const {
    std::vector<AddressEntry> results(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        results[i] = GetAddress(indices[i]);
    }
    return results;
}

}  // namespace storage
}  // namespace vdb
