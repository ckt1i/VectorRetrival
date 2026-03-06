#include "vdb/storage/cluster_store.h"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#include "vdb/simd/popcount.h"

namespace vdb {
namespace storage {

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

        // Write norm
        file_.write(reinterpret_cast<const char*>(&codes[i].norm),
                    sizeof(float));
        if (!file_.good()) {
            return Status::IOError("Failed to write norm");
        }
        current_offset_ += sizeof(float);
    }

    info_.rabitq_data_length =
        static_cast<uint32_t>(current_offset_ - info_.rabitq_data_offset);

    // --- Write norms array separately (for batch loading) ---
    info_.norms_offset = current_offset_;

    for (uint32_t i = 0; i < N; ++i) {
        file_.write(reinterpret_cast<const char*>(&codes[i].norm),
                    sizeof(float));
        if (!file_.good()) {
            return Status::IOError("Failed to write norm array entry");
        }
        current_offset_ += sizeof(float);
    }

    info_.norms_length =
        static_cast<uint32_t>(current_offset_ - info_.norms_offset);

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
// Finalize
// ============================================================================

Status ClusterStoreWriter::Finalize(const std::string& data_file_path) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("Already finalized");
    }

    info_.data_file_path = data_file_path;

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
                                     std::vector<uint64_t>& out_code,
                                     float& out_norm) const {
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

    // Read norm (immediately after code words)
    n = ::pread(fd_, &out_norm, sizeof(float),
                static_cast<off_t>(code_offset + nwords * sizeof(uint64_t)));
    if (n != sizeof(float)) {
        return Status::IOError("Failed to read norm");
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
        float norm;
        VDB_RETURN_IF_ERROR(LoadCode(indices[i], code, norm));

        out_codes[i].code = std::move(code);
        out_codes[i].norm = norm;
        out_codes[i].sum_x = simd::PopcountTotal(
            out_codes[i].code.data(),
            static_cast<uint32_t>(out_codes[i].code.size()));
    }

    return Status::OK();
}

// ============================================================================
// LoadAllNorms
// ============================================================================

Status ClusterStoreReader::LoadAllNorms(std::vector<float>& out) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    out.resize(info_.num_records);
    const uint32_t bytes = info_.num_records * sizeof(float);

    ssize_t n = ::pread(fd_, out.data(), bytes,
                        static_cast<off_t>(info_.norms_offset));
    if (n != static_cast<ssize_t>(bytes)) {
        return Status::IOError("Failed to read norms array");
    }

    return Status::OK();
}

// ============================================================================
// GetAddress / GetAddresses
// ============================================================================

AddressEntry ClusterStoreReader::GetAddress(uint32_t record_idx) const {
    return AddressColumn::Lookup(info_.address_blocks, record_idx);
}

std::vector<AddressEntry> ClusterStoreReader::GetAddresses(
    const std::vector<uint32_t>& indices) const {
    return AddressColumn::BatchLookup(info_.address_blocks, indices);
}

}  // namespace storage
}  // namespace vdb
