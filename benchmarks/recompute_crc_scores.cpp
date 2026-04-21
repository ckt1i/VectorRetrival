#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "vdb/index/crc_calibrator.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/npy_reader.h"
#include "vdb/storage/pack_codes.h"

using namespace vdb;
using namespace vdb::index;

namespace {

std::string GetStringArg(int argc, char* argv[], const char* name,
                         const std::string& default_val = {}) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return default_val;
}

int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == name) return std::atoi(argv[i + 1]);
    }
    return default_val;
}

struct OffsetWithCluster {
    uint64_t offset = 0;
    uint32_t cid = 0;
    uint32_t local_idx = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
    const std::string data_dir = GetStringArg(argc, argv, "--dataset");
    const std::string index_dir = GetStringArg(argc, argv, "--index-dir");
    const int queries = GetIntArg(argc, argv, "--queries", 2000);
    const int top_k = GetIntArg(argc, argv, "--topk", 10);

    if (data_dir.empty() || index_dir.empty()) {
        std::fprintf(stderr,
                     "usage: %s --dataset <dir> --index-dir <dir> [--queries 2000] [--topk 10]\n",
                     argv[0]);
        return 2;
    }

    auto qry_emb_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load query_embeddings: %s\n",
                     qry_emb_or.status().ToString().c_str());
        return 3;
    }
    const auto& qry_emb = qry_emb_or.value();
    const uint32_t Q = std::min<uint32_t>(static_cast<uint32_t>(queries), qry_emb.rows);

    IvfIndex index;
    Status s = index.Open(index_dir);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to open index: %s\n", s.ToString().c_str());
        return 4;
    }

    storage::Segment& segment = index.segment();
    s = segment.PreloadAllClusters();
    if (!s.ok()) {
        std::fprintf(stderr, "PreloadAllClusters failed: %s\n", s.ToString().c_str());
        return 5;
    }

    const auto& cluster_ids = index.cluster_ids();
    std::vector<OffsetWithCluster> all_offsets;
    all_offsets.reserve(static_cast<size_t>(segment.total_records()));
    for (uint32_t cid : cluster_ids) {
        const auto* resident = segment.GetResidentClusterView(cid);
        if (resident == nullptr) {
            std::fprintf(stderr, "Missing resident cluster view for cid=%u\n", cid);
            return 6;
        }
        for (uint32_t i = 0; i < resident->num_records; ++i) {
            AddressEntry addr = resident->addresses_are_raw_v2
                ? storage::AddressColumn::DecodeRawEntryV2(resident->raw_addresses[i],
                                                           resident->address_page_size)
                : resident->decoded_addresses[i];
            all_offsets.push_back({addr.offset, cid, i});
        }
    }
    std::sort(all_offsets.begin(), all_offsets.end(),
              [](const auto& a, const auto& b) { return a.offset < b.offset; });

    std::vector<std::vector<uint32_t>> global_ids(index.nlist());
    for (uint32_t cid : cluster_ids) {
        const auto* resident = segment.GetResidentClusterView(cid);
        global_ids[cid].resize(resident->num_records);
    }
    for (uint32_t row = 0; row < all_offsets.size(); ++row) {
        const auto& x = all_offsets[row];
        global_ids[x.cid][x.local_idx] = row;
    }

    std::vector<std::vector<ClusterData::FsBlock>> fs_blocks(index.nlist());
    std::vector<ClusterData> clusters(index.nlist());
    for (uint32_t cid : cluster_ids) {
        const auto* resident = segment.GetResidentClusterView(cid);
        const auto* parsed = segment.GetResidentParsedCluster(cid);
        if (resident == nullptr || parsed == nullptr) {
            std::fprintf(stderr, "Missing resident parsed cluster for cid=%u\n", cid);
            return 7;
        }

        fs_blocks[cid].resize(parsed->num_fastscan_blocks);
        const uint32_t packed_sz = storage::FastScanPackedSize(index.dim());
        for (uint32_t b = 0; b < parsed->num_fastscan_blocks; ++b) {
            const uint8_t* block =
                parsed->fastscan_blocks + static_cast<size_t>(b) * parsed->fastscan_block_size;
            fs_blocks[cid][b].packed = block;
            fs_blocks[cid][b].norms =
                reinterpret_cast<const float*>(block + packed_sz);
            fs_blocks[cid][b].count =
                std::min<uint32_t>(32u, parsed->num_records - b * 32u);
        }

        clusters[cid].vectors = nullptr;
        clusters[cid].ids = global_ids[cid].data();
        clusters[cid].count = resident->num_records;
        clusters[cid].fs_blocks = fs_blocks[cid].data();
        clusters[cid].num_fs_blocks = static_cast<uint32_t>(fs_blocks[cid].size());
    }

    auto scores = CrcCalibrator::ComputeScoresRaBitQ(
        qry_emb.data.data(), Q, index.dim(), index.centroid(0), index.nlist(),
        clusters, static_cast<uint32_t>(top_k), index.rotation());

    s = CrcCalibrator::WriteScores(index_dir + "/crc_scores.bin", scores,
                                   index.nlist(), static_cast<uint32_t>(top_k));
    if (!s.ok()) {
        std::fprintf(stderr, "WriteScores failed: %s\n", s.ToString().c_str());
        return 8;
    }

    std::printf("wrote %u query scores to %s/crc_scores.bin\n", Q, index_dir.c_str());
    return 0;
}
