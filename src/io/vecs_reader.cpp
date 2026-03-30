#include "vdb/io/vecs_reader.h"

#include <cstring>
#include <fstream>

namespace vdb {
namespace io {

namespace {

bool EndsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

// =============================================================================
// LoadFvecs
// =============================================================================

StatusOr<NpyArrayFloat> LoadFvecs(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return Status::IOError("Failed to open fvecs file: " + path);
    }

    auto file_size = static_cast<size_t>(f.tellg());
    f.seekg(0);

    if (file_size < 4) {
        return Status::Corruption("fvecs file too small: " + path);
    }

    // Read dim from first record
    int32_t dim;
    f.read(reinterpret_cast<char*>(&dim), 4);
    if (dim <= 0) {
        return Status::Corruption("Invalid dim in fvecs: " + std::to_string(dim));
    }

    size_t record_size = 4 + static_cast<size_t>(dim) * sizeof(float);
    if (file_size % record_size != 0) {
        return Status::Corruption(
            "fvecs file size not a multiple of record size (" +
            std::to_string(record_size) + "): " + path);
    }

    uint32_t rows = static_cast<uint32_t>(file_size / record_size);

    NpyArrayFloat arr;
    arr.rows = rows;
    arr.cols = static_cast<uint32_t>(dim);
    arr.data.resize(static_cast<size_t>(rows) * dim);

    // Seek back to start and read all records
    f.seekg(0);
    for (uint32_t i = 0; i < rows; ++i) {
        int32_t rec_dim;
        f.read(reinterpret_cast<char*>(&rec_dim), 4);
        if (rec_dim != dim) {
            return Status::Corruption(
                "Inconsistent dim at record " + std::to_string(i) +
                ": expected " + std::to_string(dim) + ", got " +
                std::to_string(rec_dim));
        }
        f.read(reinterpret_cast<char*>(arr.data.data() + static_cast<size_t>(i) * dim),
               dim * sizeof(float));
    }

    if (!f.good()) {
        return Status::IOError("Error reading fvecs data from: " + path);
    }

    return arr;
}

// =============================================================================
// LoadIvecs
// =============================================================================

StatusOr<VecsArrayInt32> LoadIvecs(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return Status::IOError("Failed to open ivecs file: " + path);
    }

    auto file_size = static_cast<size_t>(f.tellg());
    f.seekg(0);

    if (file_size < 4) {
        return Status::Corruption("ivecs file too small: " + path);
    }

    // Read dim from first record
    int32_t dim;
    f.read(reinterpret_cast<char*>(&dim), 4);
    if (dim <= 0) {
        return Status::Corruption("Invalid dim in ivecs: " + std::to_string(dim));
    }

    size_t record_size = 4 + static_cast<size_t>(dim) * sizeof(int32_t);
    if (file_size % record_size != 0) {
        return Status::Corruption(
            "ivecs file size not a multiple of record size (" +
            std::to_string(record_size) + "): " + path);
    }

    uint32_t rows = static_cast<uint32_t>(file_size / record_size);

    VecsArrayInt32 arr;
    arr.rows = rows;
    arr.cols = static_cast<uint32_t>(dim);
    arr.data.resize(static_cast<size_t>(rows) * dim);

    // Seek back to start and read all records
    f.seekg(0);
    for (uint32_t i = 0; i < rows; ++i) {
        int32_t rec_dim;
        f.read(reinterpret_cast<char*>(&rec_dim), 4);
        if (rec_dim != dim) {
            return Status::Corruption(
                "Inconsistent dim at record " + std::to_string(i) +
                ": expected " + std::to_string(dim) + ", got " +
                std::to_string(rec_dim));
        }
        f.read(reinterpret_cast<char*>(arr.data.data() + static_cast<size_t>(i) * dim),
               dim * sizeof(int32_t));
    }

    if (!f.good()) {
        return Status::IOError("Error reading ivecs data from: " + path);
    }

    return arr;
}

// =============================================================================
// LoadVectors — extension-based dispatch
// =============================================================================

StatusOr<NpyArrayFloat> LoadVectors(const std::string& path) {
    if (EndsWith(path, ".fvecs")) return LoadFvecs(path);
    if (EndsWith(path, ".npy"))   return LoadNpyFloat32(path);
    return Status::InvalidArgument("Unsupported vector format: " + path);
}

}  // namespace io
}  // namespace vdb
