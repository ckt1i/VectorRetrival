/// Compare Stage2 ExRaBitQ payloads between two built indexes.
///
/// Usage:
///   bench_compare_exrabitq_payloads --index-a /path/to/v10_index \
///                                   --index-b /path/to/v11_index
///
/// The tool compares per-record Stage2 payloads:
///   - ex_code
///   - ex_sign_packed
///   - xipnorm
/// across all clusters/records and reports mismatch statistics.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/storage/cluster_store.h"

using namespace vdb;

namespace {

std::string GetArg(int argc, char* argv[], const char* name,
                   const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

void PrintUsage() {
    std::fprintf(stderr,
                 "Usage: bench_compare_exrabitq_payloads "
                 "--index-a <dir> --index-b <dir>\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string index_a = GetArg(argc, argv, "--index-a");
    const std::string index_b = GetArg(argc, argv, "--index-b");
    if (index_a.empty() || index_b.empty()) {
        PrintUsage();
        return 1;
    }

    storage::ClusterStoreReader reader_a;
    storage::ClusterStoreReader reader_b;
    Status s = reader_a.Open(index_a + "/cluster.clu", false);
    if (!s.ok()) {
        std::fprintf(stderr, "Open A failed: %s\n", s.ToString().c_str());
        return 1;
    }
    s = reader_b.Open(index_b + "/cluster.clu", false);
    if (!s.ok()) {
        std::fprintf(stderr, "Open B failed: %s\n", s.ToString().c_str());
        return 1;
    }

    const auto clusters_a = reader_a.cluster_ids();
    const auto clusters_b = reader_b.cluster_ids();
    if (clusters_a != clusters_b) {
        std::fprintf(stderr, "Cluster id sets differ\n");
        return 1;
    }

    uint64_t total_records = 0;
    uint64_t code_mismatch = 0;
    uint64_t sign_mismatch = 0;
    uint64_t xipnorm_mismatch = 0;
    uint64_t msb_code_mismatch = 0;
    uint64_t norm_mismatch = 0;
    uint64_t sumx_mismatch = 0;
    float max_xipnorm_abs_diff = 0.0f;
    float max_norm_abs_diff = 0.0f;
    uint32_t first_bad_cluster = 0;
    uint32_t first_bad_idx = 0;
    bool first_bad_recorded = false;
    std::vector<uint8_t> first_a_code;
    std::vector<uint8_t> first_b_code;
    std::vector<uint8_t> first_a_sign;
    std::vector<uint8_t> first_b_sign;
    float first_a_xip = 0.0f;
    float first_b_xip = 0.0f;

    for (uint32_t cid : clusters_a) {
        const uint32_t n_a = reader_a.GetNumRecords(cid);
        const uint32_t n_b = reader_b.GetNumRecords(cid);
        if (n_a != n_b) {
            std::fprintf(stderr, "Cluster %u record count mismatch: %u vs %u\n",
                         cid, n_a, n_b);
            return 1;
        }

        s = reader_a.EnsureClusterLoaded(cid);
        if (!s.ok()) {
            std::fprintf(stderr, "EnsureClusterLoaded A failed for cluster %u: %s\n",
                         cid, s.ToString().c_str());
            return 1;
        }
        s = reader_b.EnsureClusterLoaded(cid);
        if (!s.ok()) {
            std::fprintf(stderr, "EnsureClusterLoaded B failed for cluster %u: %s\n",
                         cid, s.ToString().c_str());
            return 1;
        }

        std::vector<uint32_t> indices(n_a);
        for (uint32_t i = 0; i < n_a; ++i) indices[i] = i;

        std::vector<rabitq::RaBitQCode> codes_a;
        std::vector<rabitq::RaBitQCode> codes_b;
        s = reader_a.LoadCodes(cid, indices, codes_a);
        if (!s.ok()) {
            std::fprintf(stderr, "LoadCodes A failed for cluster %u: %s\n",
                         cid, s.ToString().c_str());
            return 1;
        }
        s = reader_b.LoadCodes(cid, indices, codes_b);
        if (!s.ok()) {
            std::fprintf(stderr, "LoadCodes B failed for cluster %u: %s\n",
                         cid, s.ToString().c_str());
            return 1;
        }

        for (uint32_t i = 0; i < n_a; ++i) {
            ++total_records;
            const auto& a = codes_a[i];
            const auto& b = codes_b[i];

            const bool msb_diff = (a.code != b.code);
            const bool code_diff = (a.ex_code != b.ex_code);
            const bool sign_diff = (a.ex_sign_packed != b.ex_sign_packed);
            const float xdiff = std::abs(a.xipnorm - b.xipnorm);
            const float ndiff = std::abs(a.norm - b.norm);
            const bool xip_diff = xdiff > 1e-6f;
            const bool norm_diff = ndiff > 1e-6f;
            const bool sumx_diff = (a.sum_x != b.sum_x);

            msb_code_mismatch += msb_diff ? 1 : 0;
            code_mismatch += code_diff ? 1 : 0;
            sign_mismatch += sign_diff ? 1 : 0;
            xipnorm_mismatch += xip_diff ? 1 : 0;
            norm_mismatch += norm_diff ? 1 : 0;
            sumx_mismatch += sumx_diff ? 1 : 0;
            max_xipnorm_abs_diff = std::max(max_xipnorm_abs_diff, xdiff);
            max_norm_abs_diff = std::max(max_norm_abs_diff, ndiff);

            if (!first_bad_recorded &&
                (msb_diff || norm_diff || sumx_diff || code_diff || sign_diff || xip_diff)) {
                first_bad_recorded = true;
                first_bad_cluster = cid;
                first_bad_idx = i;
                first_a_code = a.ex_code;
                first_b_code = b.ex_code;
                first_a_sign = a.ex_sign_packed;
                first_b_sign = b.ex_sign_packed;
                first_a_xip = a.xipnorm;
                first_b_xip = b.xipnorm;
            }
        }
    }

    std::printf("index_a=%s\n", index_a.c_str());
    std::printf("index_b=%s\n", index_b.c_str());
    std::printf("total_records=%llu\n",
                static_cast<unsigned long long>(total_records));
    std::printf("msb_code_mismatch=%llu\n",
                static_cast<unsigned long long>(msb_code_mismatch));
    std::printf("norm_mismatch=%llu\n",
                static_cast<unsigned long long>(norm_mismatch));
    std::printf("sumx_mismatch=%llu\n",
                static_cast<unsigned long long>(sumx_mismatch));
    std::printf("code_mismatch=%llu\n",
                static_cast<unsigned long long>(code_mismatch));
    std::printf("sign_mismatch=%llu\n",
                static_cast<unsigned long long>(sign_mismatch));
    std::printf("xipnorm_mismatch=%llu\n",
                static_cast<unsigned long long>(xipnorm_mismatch));
    std::printf("max_norm_abs_diff=%.9f\n", max_norm_abs_diff);
    std::printf("max_xipnorm_abs_diff=%.9f\n", max_xipnorm_abs_diff);
    if (first_bad_recorded) {
        std::printf("first_bad_cluster=%u first_bad_idx=%u\n",
                    first_bad_cluster, first_bad_idx);
        auto dump_prefix = [](const char* name, const std::vector<uint8_t>& v) {
            std::printf("%s=", name);
            const size_t n = std::min<size_t>(v.size(), 16);
            for (size_t i = 0; i < n; ++i) {
                std::printf("%s%u", (i == 0 ? "" : ","), static_cast<unsigned>(v[i]));
            }
            std::printf("\n");
        };
        dump_prefix("first_a_code_prefix", first_a_code);
        dump_prefix("first_b_code_prefix", first_b_code);
        dump_prefix("first_a_sign_prefix", first_a_sign);
        dump_prefix("first_b_sign_prefix", first_b_sign);
        std::printf("first_a_xipnorm=%.9f\n", first_a_xip);
        std::printf("first_b_xipnorm=%.9f\n", first_b_xip);
    } else {
        std::printf("all_stage2_payloads_match=1\n");
    }
    return 0;
}
