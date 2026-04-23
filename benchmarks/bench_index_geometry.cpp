#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_estimator.h"

using namespace vdb;

static const char* GetStrArg(int argc, char* argv[], const char* name,
                             const char* default_val = "") {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return default_val;
}

static int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return std::atoi(argv[i + 1]);
        }
    }
    return default_val;
}

static double Mean(const std::vector<float>& xs) {
    if (xs.empty()) return 0.0;
    double s = 0.0;
    for (float x : xs) s += x;
    return s / static_cast<double>(xs.size());
}

static float Percentile(std::vector<float> xs, double p) {
    if (xs.empty()) return 0.0f;
    std::sort(xs.begin(), xs.end());
    const size_t idx = static_cast<size_t>(std::clamp(
        p * static_cast<double>(xs.size() - 1), 0.0,
        static_cast<double>(xs.size() - 1)));
    return xs[idx];
}

static const char* CoarseBuilderName(index::CoarseBuilder builder) {
    switch (builder) {
        case index::CoarseBuilder::SuperKMeans: return "superkmeans";
        case index::CoarseBuilder::HierarchicalSuperKMeans: return "hierarchical_superkmeans";
        case index::CoarseBuilder::FaissKMeans: return "faiss_kmeans";
        case index::CoarseBuilder::Auto: return "auto";
    }
    return "unknown";
}

int main(int argc, char* argv[]) {
    const std::string index_dir = GetStrArg(argc, argv, "--index-dir");
    const std::string dataset_dir = GetStrArg(argc, argv, "--dataset");
    const uint32_t query_count =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--queries", 200));
    const uint32_t nprobe =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 64));
    if (index_dir.empty() || dataset_dir.empty()) {
        std::fprintf(stderr, "usage: bench_index_geometry --index-dir DIR --dataset DIR [--queries N] [--nprobe N]\n");
        return 2;
    }

    index::IvfIndex index;
    auto st = index.Open(index_dir);
    if (!st.ok()) {
        std::fprintf(stderr, "open index failed: %.*s\n",
                     static_cast<int>(st.message().size()), st.message().data());
        return 1;
    }

    const Dim dim = index.dim();
    auto qry_or = io::LoadNpyFloat32(dataset_dir + "/query_embeddings.npy");
    if (!qry_or.ok()) {
        std::fprintf(stderr, "load queries failed: %.*s\n",
                     static_cast<int>(qry_or.status().message().size()),
                     qry_or.status().message().data());
        return 1;
    }
    const auto& qry = qry_or.value();
    if (qry.cols != dim) {
        std::fprintf(stderr, "unexpected query shape\n");
        return 1;
    }
    const float* queries = qry.data.data();
    uint32_t num_queries = std::min<uint32_t>(qry.rows, query_count);

    auto& seg = index.segment();
    std::vector<uint32_t> cids = seg.cluster_ids();

    std::vector<float> cluster_sizes;
    std::vector<float> cluster_rmax;
    std::vector<float> norm_oc_all;
    cluster_sizes.reserve(cids.size());
    cluster_rmax.reserve(cids.size());
    norm_oc_all.reserve(seg.total_records());

    for (uint32_t cid : cids) {
        const uint32_t n = seg.GetNumRecords(cid);
        cluster_sizes.push_back(static_cast<float>(n));
        cluster_rmax.push_back(seg.cluster_reader().GetEpsilon(cid));
        st = seg.EnsureClusterLoaded(cid);
        if (!st.ok()) {
            std::fprintf(stderr, "EnsureClusterLoaded failed for cid=%u: %.*s\n",
                         cid, static_cast<int>(st.message().size()), st.message().data());
            return 1;
        }
        std::vector<uint32_t> idxs(n);
        std::iota(idxs.begin(), idxs.end(), 0u);
        std::vector<rabitq::RaBitQCode> codes;
        st = seg.LoadCodes(cid, idxs, codes);
        if (!st.ok()) {
            std::fprintf(stderr, "LoadCodes failed for cid=%u: %.*s\n",
                         cid, static_cast<int>(st.message().size()), st.message().data());
            return 1;
        }
        for (const auto& c : codes) norm_oc_all.push_back(c.norm);
    }

    rabitq::PreparedQuery pq;
    rabitq::ClusterPreparedScratch prep_scratch;
    rabitq::RaBitQEstimator estimator(dim, index.segment().cluster_reader().rabitq_config().bits);
    std::vector<float> norm_qc_topn;
    std::vector<float> margin_factor_topn;
    norm_qc_topn.reserve(static_cast<size_t>(num_queries) * nprobe);
    margin_factor_topn.reserve(static_cast<size_t>(num_queries) * nprobe);

    for (uint32_t qi = 0; qi < num_queries; ++qi) {
        const float* q = queries + static_cast<size_t>(qi) * dim;
        auto nearest = index.FindNearestClusters(q, nprobe);
        for (ClusterID cid : nearest) {
            const float* centroid = index.centroid(cid);
            estimator.PrepareQueryInto(q, centroid, index.rotation(), &pq, &prep_scratch);
            norm_qc_topn.push_back(pq.norm_qc);
            margin_factor_topn.push_back(2.0f * pq.norm_qc * index.conann().epsilon());
        }
    }

    std::printf("index_dir=%s\n", index_dir.c_str());
    std::printf("storage_version=%u\n", seg.cluster_reader().file_version());
    std::printf("loaded_eps_ip=%.6f\n", index.conann().epsilon());
    std::printf("loaded_d_k=%.6f\n", index.conann().d_k());
    std::printf("coarse_builder=%s\n",
                CoarseBuilderName(index.coarse_builder()));
    std::printf("clusters=%zu total_records=%llu\n",
                cids.size(),
                static_cast<unsigned long long>(seg.total_records()));

    auto print_stats = [](const char* name, const std::vector<float>& xs) {
        std::printf("%s: mean=%.6f p50=%.6f p95=%.6f p99=%.6f max=%.6f\n",
                    name,
                    Mean(xs),
                    Percentile(xs, 0.50),
                    Percentile(xs, 0.95),
                    Percentile(xs, 0.99),
                    xs.empty() ? 0.0f : *std::max_element(xs.begin(), xs.end()));
    };

    print_stats("cluster_size", cluster_sizes);
    print_stats("cluster_rmax", cluster_rmax);
    print_stats("norm_oc_all", norm_oc_all);
    print_stats("norm_qc_topn", norm_qc_topn);
    print_stats("margin_factor_topn", margin_factor_topn);
    return 0;
}
