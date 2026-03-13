#include "vdb/storage/cluster_store.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "vdb/simd/popcount.h"

namespace vdb {
namespace storage {

// ============================================================================
// Constants
// ============================================================================

/// Global file header magic: "VCML" (0x4C4D4356 little-endian)
static constexpr uint32_t kGlobalMagic = 0x4C4D4356;
static constexpr uint32_t kFileVersion = 4;

/// Per-cluster block mini-trailer magic: "VCLB" (0x424C4356 little-endian)
static constexpr uint32_t kBlockMagic = 0x424C4356;

// ============================================================================
// Helper: append / read raw bytes
// ============================================================================

namespace {

inline void WriteRaw(std::fstream& f, const void* data, size_t len) {
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

template <typename T>
inline void WriteVal(std::fstream& f, T value) {
    WriteRaw(f, &value, sizeof(T));
}

template <typename T>
inline bool PreadValue(int fd, off_t offset, T& out) {
    ssize_t n = ::pread(fd, &out, sizeof(T), offset);
    return n == static_cast<ssize_t>(sizeof(T));
}

inline bool PreadBytes(int fd, off_t offset, void* out, size_t len) {
    ssize_t n = ::pread(fd, out, len, offset);
    return n == static_cast<ssize_t>(len);
}

}  // anonymous namespace

// ============================================================================
// ClusterStoreWriter
// ============================================================================

ClusterStoreWriter::ClusterStoreWriter() = default;

ClusterStoreWriter::~ClusterStoreWriter() {
    if (file_.is_open() && !finalized_) {
        file_.close();
    }
}

uint64_t ClusterStoreWriter::lookup_entry_size() const {
    // cluster_id(4) + num_records(4) + centroid(dim*4) + block_offset(8) + block_size(8)
    return 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8;
}

// ============================================================================
// Global Header Layout:
//   magic            : u32
//   version          : u32
//   num_clusters     : u32
//   dim              : u32
//   rabitq.bits      : u8
//   rabitq.block_size: u32
//   rabitq.c_factor  : f32
//   data_file_path_len: u32   (placeholder 256)
//   data_file_path   : char[256]  (zero-padded)
//   [Lookup Table follows immediately]
// ============================================================================

static constexpr uint32_t kMaxPathLen = 256;

Status ClusterStoreWriter::Open(const std::string& path,
                                 uint32_t num_clusters,
                                 Dim dim,
                                 const RaBitQConfig& rabitq_config) {
    if (file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter already open");
    }

    path_ = path;
    info_.num_clusters = num_clusters;
    info_.dim = dim;
    info_.rabitq_config = rabitq_config;
    info_.lookup_table.resize(num_clusters);
    current_cluster_index_ = 0;
    in_cluster_ = false;
    finalized_ = false;

    file_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    // --- Write global header ---
    WriteVal(file_, kGlobalMagic);
    WriteVal(file_, kFileVersion);
    WriteVal(file_, num_clusters);
    WriteVal(file_, dim);
    WriteVal(file_, rabitq_config.bits);
    WriteVal(file_, rabitq_config.block_size);
    WriteVal(file_, rabitq_config.c_factor);

    // data_file_path placeholder (length + 256 zero bytes)
    header_data_file_path_offset_ = static_cast<uint64_t>(file_.tellp());
    uint32_t zero_path_len = 0;
    WriteVal(file_, zero_path_len);
    char zero_buf[kMaxPathLen] = {0};
    WriteRaw(file_, zero_buf, kMaxPathLen);

    if (!file_.good()) {
        return Status::IOError("Failed to write global header");
    }

    // --- Record lookup table start and write zero-filled entries ---
    lookup_table_start_ = static_cast<uint64_t>(file_.tellp());

    const uint64_t entry_sz = lookup_entry_size();
    std::vector<uint8_t> zero_entry(entry_sz, 0);
    for (uint32_t i = 0; i < num_clusters; ++i) {
        WriteRaw(file_, zero_entry.data(), entry_sz);
    }

    if (!file_.good()) {
        return Status::IOError("Failed to write lookup table placeholders");
    }

    current_offset_ = static_cast<uint64_t>(file_.tellp());
    return Status::OK();
}

// ============================================================================
// BeginCluster
// ============================================================================

Status ClusterStoreWriter::BeginCluster(uint32_t cluster_id,
                                         uint32_t num_records,
                                         const float* centroid) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (in_cluster_) {
        return Status::InvalidArgument("Previous cluster not ended");
    }
    if (current_cluster_index_ >= info_.num_clusters) {
        return Status::InvalidArgument("All clusters already written");
    }

    in_cluster_ = true;
    vectors_written_ = false;
    address_written_ = false;

    // Record lookup table entry
    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.cluster_id = cluster_id;
    entry.num_records = num_records;
    entry.centroid.assign(centroid, centroid + info_.dim);

    block_start_ = current_offset_;

    return Status::OK();
}

// ============================================================================
// WriteVectors
// ============================================================================

Status ClusterStoreWriter::WriteVectors(
    const std::vector<rabitq::RaBitQCode>& codes) {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (vectors_written_) {
        return Status::InvalidArgument("Vectors already written");
    }

    for (const auto& code : codes) {
        const uint32_t code_bytes =
            static_cast<uint32_t>(code.code.size()) * sizeof(uint64_t);
        file_.write(reinterpret_cast<const char*>(code.code.data()), code_bytes);
        if (!file_.good()) {
            return Status::IOError("Failed to write RaBitQ code");
        }
        current_offset_ += code_bytes;
    }

    vectors_written_ = true;
    return Status::OK();
}

// ============================================================================
// WriteAddressBlocks
// ============================================================================

Status ClusterStoreWriter::WriteAddressBlocks(
    const EncodedAddressColumn& column) {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (!vectors_written_) {
        return Status::InvalidArgument("Must write vectors before address blocks");
    }
    if (address_written_) {
        return Status::InvalidArgument("Address blocks already written");
    }

    const auto& entry = info_.lookup_table[current_cluster_index_];
    if (column.total_records != entry.num_records) {
        return Status::InvalidArgument("Address column record count does not match cluster");
    }
    if (column.blocks.size() != column.layout.num_address_blocks) {
        return Status::InvalidArgument("Address column block count does not match layout");
    }
    if (entry.num_records > 0 && column.layout.num_address_blocks == 0) {
        return Status::InvalidArgument("Non-empty cluster requires address blocks");
    }

    current_address_column_ = column;

    for (const auto& block : column.blocks) {
        file_.write(reinterpret_cast<const char*>(block.packed.data()),
                    static_cast<std::streamsize>(block.packed.size()));
        if (!file_.good()) {
            return Status::IOError("Failed to write address block data");
        }
        current_offset_ += block.packed.size();
    }

    address_written_ = true;
    return Status::OK();
}

// ============================================================================
// EndCluster — write mini-trailer, patch lookup table
// ============================================================================

Status ClusterStoreWriter::EndCluster() {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (!address_written_) {
        return Status::InvalidArgument("Must write address blocks before EndCluster");
    }

    // --- Write block mini-trailer ---
    // Layout:
    //   page_size           : u32
    //   bit_width           : u8
    //   block_granularity   : u32
    //   fixed_packed_size   : u32
    //   last_packed_size    : u32
    //   num_address_blocks    : u32
    //   For each block:
    //     base_offset       : u32
    //   mini_trailer_size : u32
    //   block_magic       : u32

    uint64_t trailer_start = current_offset_;

    WriteVal(file_, current_address_column_.layout.page_size);
    WriteVal(file_, current_address_column_.layout.bit_width);
    WriteVal(file_, current_address_column_.layout.block_granularity);
    WriteVal(file_, current_address_column_.layout.fixed_packed_size);
    WriteVal(file_, current_address_column_.layout.last_packed_size);
    const uint32_t num_blocks = current_address_column_.layout.num_address_blocks;
    WriteVal(file_, num_blocks);

    for (const auto& block : current_address_column_.blocks) {
        WriteVal(file_, block.base_offset);
    }

    // We need the trailer size to be written BEFORE magic
    // trailer_size = (current file pos + 8) - trailer_start
    uint64_t after_blocks_pos = static_cast<uint64_t>(file_.tellp());
    uint32_t mini_trailer_size = static_cast<uint32_t>(
        (after_blocks_pos + 8) - trailer_start);
    WriteVal(file_, mini_trailer_size);
    WriteVal(file_, kBlockMagic);

    if (!file_.good()) {
        return Status::IOError("Failed to write block mini-trailer");
    }

    current_offset_ = static_cast<uint64_t>(file_.tellp());

    // --- Patch lookup table entry ---
    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.block_offset = block_start_;
    entry.block_size = current_offset_ - block_start_;

    // Seek to the lookup table entry and write it
    uint64_t entry_offset = lookup_table_start_ +
        static_cast<uint64_t>(current_cluster_index_) * lookup_entry_size();
    file_.seekp(static_cast<std::streamoff>(entry_offset));

    WriteVal(file_, entry.cluster_id);
    WriteVal(file_, entry.num_records);
    file_.write(reinterpret_cast<const char*>(entry.centroid.data()),
                static_cast<std::streamsize>(info_.dim * sizeof(float)));
    WriteVal(file_, entry.block_offset);
    WriteVal(file_, entry.block_size);

    if (!file_.good()) {
        return Status::IOError("Failed to patch lookup table entry");
    }

    // Seek back to end for next cluster
    file_.seekp(static_cast<std::streamoff>(current_offset_));

    current_cluster_index_++;
    in_cluster_ = false;
    vectors_written_ = false;
    address_written_ = false;
    current_address_column_ = EncodedAddressColumn{};

    return Status::OK();
}

// ============================================================================
// Finalize
// ============================================================================

Status ClusterStoreWriter::Finalize(const std::string& data_file_path) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("Already finalized");
    }
    if (in_cluster_) {
        return Status::InvalidArgument("Cluster not ended");
    }

    info_.data_file_path = data_file_path;

    // Patch data_file_path in the global header
    file_.seekp(static_cast<std::streamoff>(header_data_file_path_offset_));
    uint32_t path_len = static_cast<uint32_t>(data_file_path.size());
    if (path_len > kMaxPathLen) {
        return Status::InvalidArgument("data_file_path exceeds max length");
    }
    WriteVal(file_, path_len);
    char path_buf[kMaxPathLen] = {0};
    std::memcpy(path_buf, data_file_path.data(), path_len);
    WriteRaw(file_, path_buf, kMaxPathLen);

    if (!file_.good()) {
        return Status::IOError("Failed to patch data_file_path");
    }

    file_.flush();
    file_.close();
    finalized_ = true;

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
    : fd_(other.fd_),
      info_(std::move(other.info_)),
      cluster_index_(std::move(other.cluster_index_)),
      loaded_clusters_(std::move(other.loaded_clusters_)) {
    other.fd_ = -1;
}

ClusterStoreReader& ClusterStoreReader::operator=(
    ClusterStoreReader&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        info_ = std::move(other.info_);
        cluster_index_ = std::move(other.cluster_index_);
        loaded_clusters_ = std::move(other.loaded_clusters_);
        other.fd_ = -1;
    }
    return *this;
}

void ClusterStoreReader::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    loaded_clusters_.clear();
    cluster_index_.clear();
}

Status ClusterStoreReader::Open(const std::string& path) {
    if (fd_ >= 0) {
        return Status::InvalidArgument("ClusterStoreReader already open");
    }

    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    // --- Read global header ---
    off_t pos = 0;
    uint32_t magic = 0, version = 0;
    if (!PreadValue(fd_, pos, magic)) {
        Close();
        return Status::IOError("Failed to read magic");
    }
    pos += sizeof(uint32_t);

    if (magic != kGlobalMagic) {
        Close();
        return Status::Corruption("Invalid ClusterStore magic");
    }

    if (!PreadValue(fd_, pos, version)) {
        Close();
        return Status::IOError("Failed to read version");
    }
    pos += sizeof(uint32_t);

    if (version != kFileVersion) {
        Close();
        return Status::NotSupported(
            "Unsupported ClusterStore version: " + std::to_string(version));
    }

    if (!PreadValue(fd_, pos, info_.num_clusters)) {
        Close();
        return Status::IOError("Failed to read num_clusters");
    }
    pos += sizeof(uint32_t);

    if (!PreadValue(fd_, pos, info_.dim)) {
        Close();
        return Status::IOError("Failed to read dim");
    }
    pos += sizeof(uint32_t);

    if (!PreadValue(fd_, pos, info_.rabitq_config.bits)) {
        Close();
        return Status::IOError("Failed to read rabitq bits");
    }
    pos += sizeof(uint8_t);

    if (!PreadValue(fd_, pos, info_.rabitq_config.block_size)) {
        Close();
        return Status::IOError("Failed to read rabitq block_size");
    }
    pos += sizeof(uint32_t);

    if (!PreadValue(fd_, pos, info_.rabitq_config.c_factor)) {
        Close();
        return Status::IOError("Failed to read rabitq c_factor");
    }
    pos += sizeof(float);

    // data_file_path: length + 256 bytes
    uint32_t path_len = 0;
    if (!PreadValue(fd_, pos, path_len)) {
        Close();
        return Status::IOError("Failed to read path length");
    }
    pos += sizeof(uint32_t);

    if (path_len > kMaxPathLen) {
        Close();
        return Status::Corruption("data_file_path length exceeds max");
    }

    char path_buf[kMaxPathLen] = {0};
    if (!PreadBytes(fd_, pos, path_buf, kMaxPathLen)) {
        Close();
        return Status::IOError("Failed to read data_file_path");
    }
    pos += kMaxPathLen;
    info_.data_file_path.assign(path_buf, path_len);

    // --- Read lookup table ---
    info_.lookup_table.resize(info_.num_clusters);
    cluster_index_.clear();

    for (uint32_t i = 0; i < info_.num_clusters; ++i) {
        auto& entry = info_.lookup_table[i];

        if (!PreadValue(fd_, pos, entry.cluster_id)) {
            Close();
            return Status::IOError("Failed to read cluster_id");
        }
        pos += sizeof(uint32_t);

        if (!PreadValue(fd_, pos, entry.num_records)) {
            Close();
            return Status::IOError("Failed to read num_records");
        }
        pos += sizeof(uint32_t);

        entry.centroid.resize(info_.dim);
        if (!PreadBytes(fd_, pos, entry.centroid.data(),
                        info_.dim * sizeof(float))) {
            Close();
            return Status::IOError("Failed to read centroid");
        }
        pos += info_.dim * sizeof(float);

        if (!PreadValue(fd_, pos, entry.block_offset)) {
            Close();
            return Status::IOError("Failed to read block_offset");
        }
        pos += sizeof(uint64_t);

        if (!PreadValue(fd_, pos, entry.block_size)) {
            Close();
            return Status::IOError("Failed to read block_size");
        }
        pos += sizeof(uint64_t);

        cluster_index_[entry.cluster_id] = i;
    }

    return Status::OK();
}

// ============================================================================
// Lookup table accessors
// ============================================================================

std::vector<uint32_t> ClusterStoreReader::cluster_ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(info_.lookup_table.size());
    for (const auto& e : info_.lookup_table) {
        ids.push_back(e.cluster_id);
    }
    return ids;
}

uint32_t ClusterStoreReader::GetNumRecords(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return 0;
    return info_.lookup_table[it->second].num_records;
}

const float* ClusterStoreReader::GetCentroid(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return nullptr;
    return info_.lookup_table[it->second].centroid.data();
}

uint64_t ClusterStoreReader::total_records() const {
    uint64_t total = 0;
    for (const auto& e : info_.lookup_table) {
        total += e.num_records;
    }
    return total;
}

// ============================================================================
// EnsureClusterLoaded — lazy load a cluster's data block
// ============================================================================

Status ClusterStoreReader::EnsureClusterLoaded(uint32_t cluster_id) {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    // Already loaded?
    if (loaded_clusters_.count(cluster_id)) {
        return Status::OK();
    }

    // Find cluster in lookup table
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) {
        return Status::InvalidArgument(
            "Cluster " + std::to_string(cluster_id) + " not found");
    }

    const auto& entry = info_.lookup_table[it->second];
    ClusterData data;

    uint64_t block_end = entry.block_offset + entry.block_size;
    uint32_t mini_trailer_size = 0, block_magic = 0;

    if (!PreadValue(fd_, static_cast<off_t>(block_end - 8), mini_trailer_size)) {
        return Status::IOError("Failed to read mini_trailer_size");
    }
    if (!PreadValue(fd_, static_cast<off_t>(block_end - 4), block_magic)) {
        return Status::IOError("Failed to read block_magic");
    }
    if (block_magic != kBlockMagic) {
        return Status::Corruption("Invalid block magic");
    }

    // Read entire mini-trailer
    uint64_t trailer_start = block_end - mini_trailer_size;
    std::vector<uint8_t> trailer_buf(mini_trailer_size);
    if (!PreadBytes(fd_, static_cast<off_t>(trailer_start),
                    trailer_buf.data(), mini_trailer_size)) {
        return Status::IOError("Failed to read block mini-trailer");
    }

    // Parse mini-trailer
    size_t tpos = 0;
    auto ReadT = [&](auto& val) -> bool {
        if (tpos + sizeof(val) > trailer_buf.size()) return false;
        std::memcpy(&val, trailer_buf.data() + tpos, sizeof(val));
        tpos += sizeof(val);
        return true;
    };

    if (!ReadT(data.address_layout.page_size) ||
        !ReadT(data.address_layout.bit_width) ||
        !ReadT(data.address_layout.block_granularity) ||
        !ReadT(data.address_layout.fixed_packed_size) ||
        !ReadT(data.address_layout.last_packed_size) ||
        !ReadT(data.address_layout.num_address_blocks)) {
        return Status::Corruption("Mini-trailer: failed to read shared address layout");
    }

    const uint32_t num_blocks = data.address_layout.num_address_blocks;

    if (entry.num_records == 0) {
        if (num_blocks != 0 || data.address_layout.last_packed_size != 0) {
            return Status::Corruption("Empty cluster has invalid address layout");
        }
    } else if (num_blocks == 0) {
        return Status::Corruption("Non-empty cluster has zero address blocks");
    }

    data.address_blocks.resize(num_blocks);
    for (uint32_t i = 0; i < num_blocks; ++i) {
        if (!ReadT(data.address_blocks[i].base_offset)) {
            return Status::Corruption("Mini-trailer: failed to parse block " +
                                      std::to_string(i));
        }
    }

    uint32_t stored_trailer_size = 0;
    uint32_t stored_block_magic = 0;
    if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
        return Status::Corruption("Mini-trailer: failed to read trailer footer");
    }
    if (stored_trailer_size != mini_trailer_size || stored_block_magic != kBlockMagic) {
        return Status::Corruption("Mini-trailer footer mismatch");
    }
    if (tpos != trailer_buf.size()) {
        return Status::Corruption("Mini-trailer has trailing bytes");
    }

    if (num_blocks > 0 && data.address_layout.block_granularity == 0) {
        return Status::Corruption("Invalid address block granularity");
    }

    data.codes_offset = entry.block_offset;
    data.codes_length = entry.num_records * code_entry_size();

    const uint64_t address_payload_offset = entry.block_offset + data.codes_length;
    if (address_payload_offset > block_end || data.codes_length > entry.block_size) {
        return Status::Corruption("Cluster block shorter than RaBitQ code region");
    }

    uint64_t expected_payload_bytes = 0;
    if (num_blocks > 0) {
        expected_payload_bytes =
            static_cast<uint64_t>(num_blocks - 1) * data.address_layout.fixed_packed_size +
            data.address_layout.last_packed_size;
    }
    const uint64_t payload_and_trailer = entry.block_size - data.codes_length;
    if (payload_and_trailer < mini_trailer_size) {
        return Status::Corruption("Cluster block shorter than trailer");
    }
    const uint64_t actual_payload_bytes = payload_and_trailer - mini_trailer_size;
    if (actual_payload_bytes != expected_payload_bytes) {
        return Status::Corruption("Address payload size mismatch");
    }

    // --- Phase 1: pread address block packed data ---
    uint64_t addr_read_offset = address_payload_offset;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        auto& block = data.address_blocks[i];
        const uint32_t packed_size =
            AddressColumn::BlockPackedSize(data.address_layout, i);
        block.packed.resize(packed_size);
        if (packed_size == 0) continue;

        if (!PreadBytes(fd_, static_cast<off_t>(addr_read_offset),
                        block.packed.data(), packed_size)) {
            return Status::IOError("Failed to read address block packed data");
        }
        addr_read_offset += packed_size;
    }

    // --- Phase 2: SIMD decode all addresses ---
    VDB_RETURN_IF_ERROR(AddressColumn::DecodeBatchBlocks(
        data.address_layout, data.address_blocks, entry.num_records,
        data.decoded_addresses));

    loaded_clusters_[cluster_id] = std::move(data);
    return Status::OK();
}

// ============================================================================
// GetAddress / GetAddresses
// ============================================================================

AddressEntry ClusterStoreReader::GetAddress(uint32_t cluster_id,
                                             uint32_t record_idx) const {
    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end() ||
        record_idx >= it->second.decoded_addresses.size()) {
        return AddressEntry{0, 0};
    }
    return it->second.decoded_addresses[record_idx];
}

std::vector<AddressEntry> ClusterStoreReader::GetAddresses(
    uint32_t cluster_id,
    const std::vector<uint32_t>& indices) const {
    std::vector<AddressEntry> results(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        results[i] = GetAddress(cluster_id, indices[i]);
    }
    return results;
}

// ============================================================================
// LoadCode / LoadCodes
// ============================================================================

Status ClusterStoreReader::LoadCode(uint32_t cluster_id,
                                     uint32_t record_idx,
                                     std::vector<uint64_t>& out_code) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) {
        return Status::InvalidArgument(
            "Cluster not loaded — call EnsureClusterLoaded first");
    }

    auto cid_it = cluster_index_.find(cluster_id);
    if (cid_it == cluster_index_.end()) {
        return Status::InvalidArgument("Cluster not found");
    }

    const auto& entry = info_.lookup_table[cid_it->second];
    if (record_idx >= entry.num_records) {
        return Status::InvalidArgument("Record index out of range");
    }

    const uint32_t nwords = num_code_words();
    const uint32_t entry_sz = code_entry_size();

    uint64_t code_offset = it->second.codes_offset +
                           static_cast<uint64_t>(record_idx) * entry_sz;

    out_code.resize(nwords);
    if (!PreadBytes(fd_, static_cast<off_t>(code_offset),
                    out_code.data(), nwords * sizeof(uint64_t))) {
        return Status::IOError("Failed to read RaBitQ code");
    }

    return Status::OK();
}

Status ClusterStoreReader::LoadCodes(
    uint32_t cluster_id,
    const std::vector<uint32_t>& indices,
    std::vector<rabitq::RaBitQCode>& out_codes) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    out_codes.resize(indices.size());

    for (size_t i = 0; i < indices.size(); ++i) {
        std::vector<uint64_t> code;
        VDB_RETURN_IF_ERROR(LoadCode(cluster_id, indices[i], code));

        out_codes[i].code = std::move(code);
        out_codes[i].norm = 0.0f;
        out_codes[i].sum_x = simd::PopcountTotal(
            out_codes[i].code.data(),
            static_cast<uint32_t>(out_codes[i].code.size()));
    }

    return Status::OK();
}

}  // namespace storage
}  // namespace vdb
