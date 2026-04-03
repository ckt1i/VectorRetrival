#include "vdb/storage/cluster_store.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "vdb/simd/popcount.h"
#include "vdb/storage/pack_codes.h"

namespace vdb {
namespace storage {

// ============================================================================
// Constants
// ============================================================================

/// Global file header magic: "VCML" (0x4C4D4356 little-endian)
static constexpr uint32_t kGlobalMagic = 0x4C4D4356;
static constexpr uint32_t kFileVersion = 8;
static constexpr uint32_t kFileVersionV7 = 7;
static constexpr uint32_t kAlignSize = 4096;

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

inline uint64_t RoundUp4K(uint64_t v) {
    return (v + kAlignSize - 1) & ~(static_cast<uint64_t>(kAlignSize) - 1);
}

inline void PadTo4K(std::fstream& f, uint64_t& offset) {
    uint64_t aligned = RoundUp4K(offset);
    if (aligned > offset) {
        uint64_t pad = aligned - offset;
        char zeros[kAlignSize] = {0};
        while (pad > 0) {
            uint64_t chunk = std::min(pad, static_cast<uint64_t>(kAlignSize));
            f.write(zeros, static_cast<std::streamsize>(chunk));
            pad -= chunk;
        }
        offset = aligned;
    }
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
    // cluster_id(4) + num_records(4) + epsilon(4) + centroid(dim*4)
    // + block_offset(8) + block_size(8)
    // + num_fastscan_blocks(4) + exrabitq_region_offset(4)  [v7]
    return 4 + 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8 + 4 + 4;
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

    // Pad to 4KB boundary so first cluster block starts aligned
    PadTo4K(file_, current_offset_);

    return Status::OK();
}

// ============================================================================
// BeginCluster
// ============================================================================

Status ClusterStoreWriter::BeginCluster(uint32_t cluster_id,
                                         uint32_t num_records,
                                         const float* centroid,
                                         float epsilon) {
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
    entry.epsilon = epsilon;
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

    const uint32_t N = static_cast<uint32_t>(codes.size());
    const uint32_t dim = info_.dim;
    const uint32_t num_blocks = (N + 31) / 32;
    const uint32_t packed_size = FastScanPackedSize(dim);
    const uint32_t block_bytes = FastScanBlockSize(dim);

    current_num_fastscan_blocks_ = num_blocks;

    // --- Region 1: FastScan Blocks ---
    std::vector<uint8_t> packed_buf(packed_size, 0);

    for (uint32_t b = 0; b < num_blocks; ++b) {
        uint32_t start = b * 32;
        uint32_t count = std::min(32u, N - start);

        // Pack sign bits for this block of 32
        PackSignBitsForFastScan(&codes[start], count, dim, packed_buf.data());

        // Write packed codes
        WriteRaw(file_, packed_buf.data(), packed_size);

        // Write norm_oc factors (32 floats, zero-padded for last block)
        float norms[32] = {0};
        for (uint32_t j = 0; j < count; ++j) {
            norms[j] = codes[start + j].norm;
        }
        WriteRaw(file_, norms, 32 * sizeof(float));

        if (!file_.good()) {
            return Status::IOError("Failed to write FastScan block");
        }
        current_offset_ += block_bytes;
    }

    // Track Region 2 offset within the cluster block
    current_exrabitq_region_offset_ = static_cast<uint32_t>(
        current_offset_ - block_start_);

    // --- Region 2: ExRaBitQ Entries (only when bits > 1) ---
    if (info_.rabitq_config.bits > 1) {
        for (const auto& code : codes) {
            if (code.ex_code.size() != dim || code.ex_sign.size() != dim) {
                return Status::InvalidArgument(
                    "ExRaBitQ code/sign size mismatch with dim");
            }
            WriteRaw(file_, code.ex_code.data(), dim);
            WriteRaw(file_, code.ex_sign.data(), dim);
            WriteVal(file_, code.xipnorm);
            if (!file_.good()) {
                return Status::IOError("Failed to write ExRaBitQ entry");
            }
            current_offset_ += 2 * dim + sizeof(float);
        }
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

    // --- Patch lookup table entry (before padding) ---
    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.block_offset = block_start_;
    entry.block_size = current_offset_ - block_start_;
    entry.num_fastscan_blocks = current_num_fastscan_blocks_;
    entry.exrabitq_region_offset = current_exrabitq_region_offset_;

    // Seek to the lookup table entry and write it
    uint64_t entry_offset = lookup_table_start_ +
        static_cast<uint64_t>(current_cluster_index_) * lookup_entry_size();
    file_.seekp(static_cast<std::streamoff>(entry_offset));

    WriteVal(file_, entry.cluster_id);
    WriteVal(file_, entry.num_records);
    WriteVal(file_, entry.epsilon);
    file_.write(reinterpret_cast<const char*>(entry.centroid.data()),
                static_cast<std::streamsize>(info_.dim * sizeof(float)));
    WriteVal(file_, entry.block_offset);
    WriteVal(file_, entry.block_size);
    WriteVal(file_, entry.num_fastscan_blocks);
    WriteVal(file_, entry.exrabitq_region_offset);

    if (!file_.good()) {
        return Status::IOError("Failed to patch lookup table entry");
    }

    // Seek back to end for next cluster, pad to 4KB boundary
    file_.seekp(static_cast<std::streamoff>(current_offset_));
    PadTo4K(file_, current_offset_);

    current_cluster_index_++;
    in_cluster_ = false;
    vectors_written_ = false;
    address_written_ = false;
    current_address_column_ = EncodedAddressColumn{};
    current_num_fastscan_blocks_ = 0;
    current_exrabitq_region_offset_ = 0;

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

Status ClusterStoreReader::Open(const std::string& path, bool use_direct_io) {
    if (fd_ >= 0) {
        return Status::InvalidArgument("ClusterStoreReader already open");
    }

    int flags = O_RDONLY;
    if (use_direct_io) flags |= O_DIRECT;
    fd_ = ::open(path.c_str(), flags);
    if (fd_ < 0) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    // --- Read global header (aligned for O_DIRECT compatibility) ---
    // Header is < 4KB. Read first 4KB in one aligned read, parse from buffer.
    {
        uint8_t* hdr_buf = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, kAlignSize));
        if (!hdr_buf) { Close(); return Status::IOError("aligned_alloc failed for header"); }
        ssize_t n = ::pread(fd_, hdr_buf, kAlignSize, 0);
        if (n < 285) {  // minimum header size
            std::free(hdr_buf);
            Close();
            return Status::IOError("Failed to read header");
        }

        const uint8_t* p = hdr_buf;
        uint32_t magic = 0, version = 0;
        std::memcpy(&magic, p, 4); p += 4;
        if (magic != kGlobalMagic) {
            std::free(hdr_buf);
            Close();
            return Status::Corruption("Invalid ClusterStore magic");
        }
        std::memcpy(&version, p, 4); p += 4;
        if (version != kFileVersion && version != kFileVersionV7) {
            std::free(hdr_buf);
            Close();
            return Status::NotSupported(
                "Unsupported ClusterStore version: " + std::to_string(version));
        }
        bool is_v8_local = (version == kFileVersion);

        std::memcpy(&info_.num_clusters, p, 4); p += 4;
        std::memcpy(&info_.dim, p, 4); p += 4;
        std::memcpy(&info_.rabitq_config.bits, p, 1); p += 1;
        std::memcpy(&info_.rabitq_config.block_size, p, 4); p += 4;
        std::memcpy(&info_.rabitq_config.c_factor, p, 4); p += 4;

        uint32_t path_len = 0;
        std::memcpy(&path_len, p, 4); p += 4;
        if (path_len > kMaxPathLen) {
            std::free(hdr_buf);
            Close();
            return Status::Corruption("data_file_path length exceeds max");
        }
        info_.data_file_path.assign(reinterpret_cast<const char*>(p), path_len);
        p += kMaxPathLen;

        off_t pos = static_cast<off_t>(p - hdr_buf);
        std::free(hdr_buf);

    // --- Read lookup table (bulk pread, aligned) ---
    info_.lookup_table.resize(info_.num_clusters);
    cluster_index_.clear();

    const uint64_t entry_sz = 4 + 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8 + 4 + 4;
    const uint64_t total_lookup_size = static_cast<uint64_t>(info_.num_clusters) * entry_sz;
    // Aligned buffer and read length for O_DIRECT
    uint64_t aligned_lookup_size = RoundUp4K(total_lookup_size);
    uint8_t* lookup_raw = static_cast<uint8_t*>(
        std::aligned_alloc(kAlignSize, aligned_lookup_size));
    if (!lookup_raw) { Close(); return Status::IOError("aligned_alloc failed for lookup"); }
    // pos may not be 4KB-aligned for v7 files, but O_DIRECT requires aligned offset.
    // For v7 + O_DIRECT this would fail. We align the read offset down and adjust.
    off_t aligned_pos = pos & ~(static_cast<off_t>(kAlignSize) - 1);
    off_t pos_delta = pos - aligned_pos;
    uint64_t aligned_read = RoundUp4K(total_lookup_size + static_cast<uint64_t>(pos_delta));
    uint8_t* aligned_read_buf = static_cast<uint8_t*>(
        std::aligned_alloc(kAlignSize, aligned_read));
    if (!aligned_read_buf) { std::free(lookup_raw); Close(); return Status::IOError("aligned_alloc failed"); }
    ssize_t nr = ::pread(fd_, aligned_read_buf, aligned_read, aligned_pos);
    if (nr < static_cast<ssize_t>(total_lookup_size + pos_delta)) {
        std::free(aligned_read_buf); std::free(lookup_raw);
        Close();
        return Status::IOError("Failed to bulk read lookup table");
    }
    std::memcpy(lookup_raw, aligned_read_buf + pos_delta, total_lookup_size);
    std::free(aligned_read_buf);

    const uint8_t* ptr = lookup_raw;
    for (uint32_t i = 0; i < info_.num_clusters; ++i) {
        auto& entry = info_.lookup_table[i];

        std::memcpy(&entry.cluster_id, ptr, 4); ptr += 4;
        std::memcpy(&entry.num_records, ptr, 4); ptr += 4;
        std::memcpy(&entry.epsilon, ptr, 4); ptr += 4;

        entry.centroid.resize(info_.dim);
        std::memcpy(entry.centroid.data(), ptr, info_.dim * sizeof(float));
        ptr += info_.dim * sizeof(float);

        std::memcpy(&entry.block_offset, ptr, 8); ptr += 8;
        std::memcpy(&entry.block_size, ptr, 8); ptr += 8;
        std::memcpy(&entry.num_fastscan_blocks, ptr, 4); ptr += 4;
        std::memcpy(&entry.exrabitq_region_offset, ptr, 4); ptr += 4;

        cluster_index_[entry.cluster_id] = i;
    }
    std::free(lookup_raw);
    pos += static_cast<off_t>(total_lookup_size);

    // v8: skip padding after lookup table to 4KB boundary
    if (is_v8_local) {
        pos = static_cast<off_t>(RoundUp4K(static_cast<uint64_t>(pos)));
    }

    }  // end header reading scope

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

float ClusterStoreReader::GetEpsilon(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return 0.0f;
    return info_.lookup_table[it->second].epsilon;
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
    // v7: codes_length = Region 1 (FastScan blocks) + Region 2 (ExRaBitQ entries)
    const uint32_t region1_size = entry.num_fastscan_blocks * fastscan_block_bytes();
    const uint32_t region2_size = entry.num_records * exrabitq_entry_size();
    data.codes_length = region1_size + region2_size;

    // Cache full codes region (Region 1 + Region 2) in memory for zero-syscall Probe access
    if (data.codes_length > 0) {
        data.codes_buffer.resize(data.codes_length);
        if (!PreadBytes(fd_, static_cast<off_t>(data.codes_offset),
                        data.codes_buffer.data(), data.codes_length)) {
            return Status::IOError("Failed to read codes buffer");
        }
    }

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
// Async query support (Phase 8)
// ============================================================================

std::optional<query::ClusterBlockLocation>
ClusterStoreReader::GetBlockLocation(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return std::nullopt;
    const auto& entry = info_.lookup_table[it->second];
    return query::ClusterBlockLocation{
        entry.block_offset, entry.block_size, entry.num_records};
}

Status ClusterStoreReader::ParseClusterBlock(
    uint32_t cluster_id,
    query::AlignedBufPtr block_buf,
    uint64_t block_size,
    query::ParsedCluster& out) {

    auto idx_it = cluster_index_.find(cluster_id);
    if (idx_it == cluster_index_.end()) {
        return Status::InvalidArgument(
            "Cluster " + std::to_string(cluster_id) + " not found");
    }
    const auto& entry = info_.lookup_table[idx_it->second];

    if (block_size != entry.block_size) {
        return Status::InvalidArgument("block_size mismatch");
    }

    // --- Read mini-trailer from buffer (last 8 bytes = trailer_size + magic) ---
    if (block_size < 8) {
        return Status::Corruption("Block too small for trailer footer");
    }

    uint32_t mini_trailer_size = 0, block_magic = 0;
    std::memcpy(&mini_trailer_size, block_buf.get() + block_size - 8,
                sizeof(uint32_t));
    std::memcpy(&block_magic, block_buf.get() + block_size - 4,
                sizeof(uint32_t));
    if (block_magic != kBlockMagic) {
        return Status::Corruption("Invalid block magic");
    }
    if (mini_trailer_size > block_size) {
        return Status::Corruption("Mini-trailer size exceeds block");
    }

    // Read entire mini-trailer from buffer
    const uint8_t* trailer_ptr = block_buf.get() + block_size - mini_trailer_size;

    // Parse mini-trailer
    size_t tpos = 0;
    auto ReadT = [&](auto& val) -> bool {
        if (tpos + sizeof(val) > mini_trailer_size) return false;
        std::memcpy(&val, trailer_ptr + tpos, sizeof(val));
        tpos += sizeof(val);
        return true;
    };

    AddressColumnLayout address_layout;
    if (!ReadT(address_layout.page_size) ||
        !ReadT(address_layout.bit_width) ||
        !ReadT(address_layout.block_granularity) ||
        !ReadT(address_layout.fixed_packed_size) ||
        !ReadT(address_layout.last_packed_size) ||
        !ReadT(address_layout.num_address_blocks)) {
        return Status::Corruption(
            "Mini-trailer: failed to read shared address layout");
    }

    const uint32_t num_blocks = address_layout.num_address_blocks;

    if (entry.num_records == 0) {
        if (num_blocks != 0 || address_layout.last_packed_size != 0) {
            return Status::Corruption(
                "Empty cluster has invalid address layout");
        }
    } else if (num_blocks == 0) {
        return Status::Corruption("Non-empty cluster has zero address blocks");
    }

    std::vector<AddressBlock> address_blocks(num_blocks);
    for (uint32_t i = 0; i < num_blocks; ++i) {
        if (!ReadT(address_blocks[i].base_offset)) {
            return Status::Corruption(
                "Mini-trailer: failed to parse block " + std::to_string(i));
        }
    }

    uint32_t stored_trailer_size = 0;
    uint32_t stored_block_magic = 0;
    if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
        return Status::Corruption(
            "Mini-trailer: failed to read trailer footer");
    }
    if (stored_trailer_size != mini_trailer_size ||
        stored_block_magic != kBlockMagic) {
        return Status::Corruption("Mini-trailer footer mismatch");
    }
    if (tpos != mini_trailer_size) {
        return Status::Corruption("Mini-trailer has trailing bytes");
    }
    if (num_blocks > 0 && address_layout.block_granularity == 0) {
        return Status::Corruption("Invalid address block granularity");
    }

    // --- v7: Compute region sizes ---
    const uint32_t fb_size = fastscan_block_bytes();
    const uint32_t region1_size = entry.num_fastscan_blocks * fb_size;
    const uint32_t ex_entry_size = exrabitq_entry_size();
    const uint32_t region2_size = entry.num_records * ex_entry_size;
    const uint32_t codes_length = region1_size + region2_size;

    // --- Validate address payload region ---
    if (codes_length > block_size) {
        return Status::Corruption(
            "Cluster block shorter than code regions");
    }
    uint64_t expected_payload_bytes = 0;
    if (num_blocks > 0) {
        expected_payload_bytes =
            static_cast<uint64_t>(num_blocks - 1) *
                address_layout.fixed_packed_size +
            address_layout.last_packed_size;
    }
    const uint64_t payload_and_trailer = block_size - codes_length;
    if (payload_and_trailer < mini_trailer_size) {
        return Status::Corruption("Cluster block shorter than trailer");
    }
    const uint64_t actual_payload_bytes =
        payload_and_trailer - mini_trailer_size;
    if (actual_payload_bytes != expected_payload_bytes) {
        return Status::Corruption("Address payload size mismatch");
    }

    // --- Read address block packed data from buffer ---
    uint64_t addr_offset = codes_length;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        auto& block = address_blocks[i];
        const uint32_t packed_size =
            AddressColumn::BlockPackedSize(address_layout, i);
        if (packed_size > 0) {
            block.packed.assign(block_buf.get() + addr_offset,
                                block_buf.get() + addr_offset + packed_size);
        }
        addr_offset += packed_size;
    }

    // --- SIMD decode all addresses ---
    std::vector<AddressEntry> decoded;
    VDB_RETURN_IF_ERROR(AddressColumn::DecodeBatchBlocks(
        address_layout, address_blocks, entry.num_records, decoded));

    // --- Fill output (v7 dual-region) ---
    out.fastscan_blocks = block_buf.get();
    out.fastscan_block_size = fb_size;
    out.num_fastscan_blocks = entry.num_fastscan_blocks;
    out.exrabitq_entries = (ex_entry_size > 0)
        ? block_buf.get() + region1_size
        : nullptr;
    out.exrabitq_entry_size = ex_entry_size;
    out.num_records = entry.num_records;
    out.epsilon = entry.epsilon;
    out.decoded_addresses = std::move(decoded);
    out.block_buf = std::move(block_buf);

    // Legacy fields (for backward compat during transition)
    out.codes_start = out.fastscan_blocks;
    out.code_entry_size = 0;  // Not meaningful in v7

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

    // v7: Extract sign bits from packed FastScan blocks
    const uint32_t nwords = num_code_words();
    const uint32_t dim = info_.dim;
    const uint32_t block_idx = record_idx / 32;
    const uint32_t vec_in_block = record_idx % 32;
    const uint32_t fb_bytes = fastscan_block_bytes();

    const uint8_t* block_data = it->second.codes_buffer.data() +
                                 block_idx * fb_bytes;

    out_code.resize(nwords);
    UnpackSignBitsFromFastScan(block_data, vec_in_block, dim, out_code.data());

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
    const uint32_t dim = info_.dim;
    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint32_t packed_sz = fastscan_packed_size();
    const uint32_t ex_entry_sz = exrabitq_entry_size();

    auto cid_it = cluster_index_.find(cluster_id);
    if (cid_it == cluster_index_.end()) {
        return Status::InvalidArgument("Cluster not found");
    }
    const auto& entry = info_.lookup_table[cid_it->second];

    for (size_t i = 0; i < indices.size(); ++i) {
        // Extract sign code words
        std::vector<uint64_t> code;
        VDB_RETURN_IF_ERROR(LoadCode(cluster_id, indices[i], code));
        out_codes[i].code = std::move(code);

        // Read norm_oc from FastScan block factors
        const auto& cluster_data = loaded_clusters_.at(cluster_id);
        uint32_t block_idx = indices[i] / 32;
        uint32_t vec_in_block = indices[i] % 32;
        const uint8_t* block_data = cluster_data.codes_buffer.data() +
                                     block_idx * fb_bytes;
        const float* norms = reinterpret_cast<const float*>(
            block_data + packed_sz);
        out_codes[i].norm = norms[vec_in_block];

        // sum_x = popcount of the sign code
        out_codes[i].sum_x = 0;
        for (auto w : out_codes[i].code) {
            out_codes[i].sum_x += __builtin_popcountll(w);
        }

        // Read ExRaBitQ fields from Region 2 (if bits > 1)
        if (ex_entry_sz > 0) {
            uint32_t region1_size = entry.num_fastscan_blocks * fb_bytes;
            const uint8_t* ex_ptr = cluster_data.codes_buffer.data() +
                                     region1_size +
                                     static_cast<size_t>(indices[i]) * ex_entry_sz;
            out_codes[i].ex_code.assign(ex_ptr, ex_ptr + dim);
            out_codes[i].ex_sign.assign(ex_ptr + dim, ex_ptr + 2 * dim);
            std::memcpy(&out_codes[i].xipnorm, ex_ptr + 2 * dim, sizeof(float));
        }
    }

    return Status::OK();
}

// ============================================================================
// GetCodePtr — raw pointer into cached codes_buffer
// ============================================================================

const uint8_t* ClusterStoreReader::GetCodePtr(uint32_t cluster_id,
                                               uint32_t record_idx) const {
    // v7: Return pointer to the start of the FastScan block containing this vector.
    // Callers should use ParsedCluster helpers instead for structured access.
    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) return nullptr;

    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint32_t block_idx = record_idx / 32;
    const uint32_t offset = block_idx * fb_bytes;
    if (offset + fb_bytes > it->second.codes_buffer.size()) return nullptr;

    return it->second.codes_buffer.data() + offset;
}

}  // namespace storage
}  // namespace vdb
