/// export_ivf_clustering.cpp — Export reusable IVF clustering artifacts
///
/// Reads an existing IVF index plus dataset image IDs and reconstructs:
///   1. centroids.fvecs
///   2. assignments.ivecs
///
/// This enables apples-to-apples rebuilds that reuse the same clustering.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/npy_reader.h"

using namespace vdb;
using namespace vdb::index;

namespace fs = std::filesystem;

namespace {

static std::string GetStringArg(int argc, char* argv[], const char* name,
                                const std::string& default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return default_val;
}

static bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static bool WriteFvecs(const std::string& path,
                       const std::vector<float>& data,
                       uint32_t rows,
                       uint32_t dim) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    int32_t d32 = static_cast<int32_t>(dim);
    for (uint32_t i = 0; i < rows; ++i) {
        f.write(reinterpret_cast<const char*>(&d32), sizeof(int32_t));
        f.write(reinterpret_cast<const char*>(
                    data.data() + static_cast<size_t>(i) * dim),
                static_cast<std::streamsize>(dim) * sizeof(float));
    }
    return f.good();
}

static bool WriteIvecs(const std::string& path,
                       const std::vector<uint32_t>& assignments) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    int32_t one = 1;
    for (uint32_t assignment : assignments) {
        int32_t value = static_cast<int32_t>(assignment);
        f.write(reinterpret_cast<const char*>(&one), sizeof(int32_t));
        f.write(reinterpret_cast<const char*>(&value), sizeof(int32_t));
    }
    return f.good();
}

static bool ExtractPayloadId(const std::vector<Datum>& payload, int64_t* id) {
    if (payload.empty()) return false;
    if (payload[0].dtype != DType::INT64) return false;
    *id = payload[0].fixed.i64;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    std::string dataset_dir = GetStringArg(argc, argv, "--dataset", "");
    std::string output_dir = GetStringArg(argc, argv, "--output-dir", "");
    bool direct_io = HasFlag(argc, argv, "--direct-io");

    if (index_dir.empty() || dataset_dir.empty()) {
        std::fprintf(stderr,
                     "Usage: export_ivf_clustering --index-dir <dir> "
                     "--dataset <dataset_dir> [--output-dir <dir>] [--direct-io]\n");
        return 1;
    }
    if (output_dir.empty()) {
        output_dir = index_dir;
    }

    fs::create_directories(output_dir);

    auto ids_or = io::LoadNpyInt64(dataset_dir + "/image_ids.npy");
    if (!ids_or.ok()) {
        std::fprintf(stderr, "Failed to load image_ids.npy: %s\n",
                     ids_or.status().ToString().c_str());
        return 1;
    }
    const auto& image_ids = ids_or.value();

    std::unordered_map<int64_t, uint32_t> id_to_row;
    id_to_row.reserve(image_ids.count * 2);
    for (uint32_t i = 0; i < image_ids.count; ++i) {
        id_to_row[image_ids.data[i]] = i;
    }

    IvfIndex index;
    auto s = index.Open(index_dir, direct_io);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to open index: %s\n", s.ToString().c_str());
        return 1;
    }

    const uint32_t nlist = index.nlist();
    const uint32_t dim = index.dim();
    std::vector<float> centroids(static_cast<size_t>(nlist) * dim);
    for (uint32_t cid = 0; cid < nlist; ++cid) {
        std::memcpy(centroids.data() + static_cast<size_t>(cid) * dim,
                    index.centroid(cid),
                    static_cast<size_t>(dim) * sizeof(float));
    }

    std::vector<uint32_t> assignments(image_ids.count, kInvalidClusterID);
    std::vector<float> scratch_vec(dim);
    std::vector<Datum> payload;
    uint64_t assigned = 0;

    for (uint32_t cid = 0; cid < nlist; ++cid) {
        s = index.segment().EnsureClusterLoaded(cid);
        if (!s.ok()) {
            std::fprintf(stderr, "Failed to load cluster %u: %s\n",
                         cid, s.ToString().c_str());
            return 1;
        }

        const uint32_t count = index.segment().GetNumRecords(cid);
        for (uint32_t ridx = 0; ridx < count; ++ridx) {
            AddressEntry addr = index.segment().GetAddress(cid, ridx);
            payload.clear();
            s = index.segment().ReadRecord(addr, scratch_vec.data(), payload);
            if (!s.ok()) {
                std::fprintf(stderr, "Failed to read record for cluster %u row %u: %s\n",
                             cid, ridx, s.ToString().c_str());
                return 1;
            }

            int64_t image_id = -1;
            if (!ExtractPayloadId(payload, &image_id)) {
                std::fprintf(stderr,
                             "Record for cluster %u row %u is missing INT64 payload id\n",
                             cid, ridx);
                return 1;
            }

            auto it = id_to_row.find(image_id);
            if (it == id_to_row.end()) {
                std::fprintf(stderr,
                             "Image ID %ld from index payload was not found in dataset image_ids.npy\n",
                             static_cast<long>(image_id));
                return 1;
            }

            uint32_t row = it->second;
            if (assignments[row] != kInvalidClusterID) {
                std::fprintf(stderr,
                             "Duplicate assignment for dataset row %u (image_id=%ld)\n",
                             row, static_cast<long>(image_id));
                return 1;
            }
            assignments[row] = cid;
            ++assigned;
        }

        if ((cid + 1) % 64 == 0 || cid + 1 == nlist) {
            Log("  Export progress: cluster %u/%u\n", cid + 1, nlist);
        }
    }

    if (assigned != image_ids.count) {
        std::fprintf(stderr,
                     "Assignment reconstruction incomplete: assigned=%lu expected=%u\n",
                     static_cast<unsigned long>(assigned), image_ids.count);
        return 1;
    }

    const std::string centroids_path = output_dir + "/centroids.fvecs";
    const std::string assignments_path = output_dir + "/assignments.ivecs";
    if (!WriteFvecs(centroids_path, centroids, nlist, dim)) {
        std::fprintf(stderr, "Failed to write %s\n", centroids_path.c_str());
        return 1;
    }
    if (!WriteIvecs(assignments_path, assignments)) {
        std::fprintf(stderr, "Failed to write %s\n", assignments_path.c_str());
        return 1;
    }

    Log("Exported centroids -> %s\n", centroids_path.c_str());
    Log("Exported assignments -> %s\n", assignments_path.c_str());
    Log("Recovered %lu assignments from %s\n",
        static_cast<unsigned long>(assigned), index_dir.c_str());
    return 0;
}
