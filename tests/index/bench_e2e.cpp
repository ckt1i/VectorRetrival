/// bench_e2e.cpp — End-to-end benchmark on real COCO datasets.
///
/// Measures: recall@{1,5,10}, query latency (avg/p50/p95/p99),
///           build time, brute-force GT time, IO/CPU pipeline utilization.
///
/// Usage:
///   bench_e2e [--dataset /path/to/coco_1k]
///
/// Output:
///   /home/zcq/VDB/test/{dataset}_{timestamp}/
///     index/         — built IVF index files
///     config.json    — parameter snapshot
///     results.json   — metrics + pipeline stats + per-query samples

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vdb/common/distance.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/jsonl_reader.h"
#include "vdb/io/npy_reader.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::query;

namespace fs = std::filesystem;

// ============================================================================
// Command-line helpers
// ============================================================================

static std::string GetStringArg(int argc, char* argv[], const char* name,
                                const std::string& default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return default_val;
}

static int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    }
    return default_val;
}

// ============================================================================
// Timestamp + dataset name
// ============================================================================

static std::string Timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", std::localtime(&t));
    return buf;
}

static std::string DatasetName(const std::string& path) {
    return fs::path(path).filename().string();
}

// ============================================================================
// JSONL field parsing (no JSON library)
// ============================================================================

static int64_t ParseImageId(std::string_view line) {
    auto pos = line.find("\"image_id\":");
    if (pos == std::string_view::npos) return -1;
    pos += 11;  // skip "image_id":
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    int64_t val = 0;
    bool neg = false;
    if (pos < line.size() && line[pos] == '-') { neg = true; ++pos; }
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        val = val * 10 + (line[pos] - '0');
        ++pos;
    }
    return neg ? -val : val;
}

static std::string ParseCaption(std::string_view line) {
    auto pos = line.find("\"caption\":");
    if (pos == std::string_view::npos) return "";
    pos += 10;  // skip "caption":
    // find opening quote
    pos = line.find('"', pos);
    if (pos == std::string_view::npos) return "";
    ++pos;  // skip opening quote
    auto end = line.find('"', pos);
    if (end == std::string_view::npos) return "";
    return std::string(line.substr(pos, end - pos));
}

// ============================================================================
// JSON output helpers (hand-crafted)
// ============================================================================

static std::string JStr(const std::string& k, const std::string& v) {
    return "\"" + k + "\": \"" + v + "\"";
}

static std::string JNum(const std::string& k, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return "\"" + k + "\": " + buf;
}

static std::string JInt(const std::string& k, int64_t v) {
    return "\"" + k + "\": " + std::to_string(v);
}

static std::string JBool(const std::string& k, bool v) {
    return "\"" + k + "\": " + (v ? "true" : "false");
}

static std::string JArr64(const std::string& k, const std::vector<int64_t>& v) {
    std::string s = "\"" + k + "\": [";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ", ";
        s += std::to_string(v[i]);
    }
    s += "]";
    return s;
}

static std::string JArrF(const std::string& k, const std::vector<float>& v) {
    std::string s = "\"" + k + "\": [";
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ", ";
        std::snprintf(buf, sizeof(buf), "%.6f", v[i]);
        s += buf;
    }
    s += "]";
    return s;
}

// ============================================================================
// Percentile (on pre-sorted vector)
// ============================================================================

static double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ============================================================================
// Recall computation
// ============================================================================

static double ComputeRecallAtK(const std::vector<int64_t>& predicted,
                                const std::vector<int64_t>& gt,
                                uint32_t K) {
    uint32_t pk = std::min(K, static_cast<uint32_t>(predicted.size()));
    uint32_t gk = std::min(K, static_cast<uint32_t>(gt.size()));
    std::unordered_set<int64_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(gk);
}

// ============================================================================
// main
// ============================================================================

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

int main(int argc, char* argv[]) {
    std::string data_dir = GetStringArg(argc, argv, "--dataset",
                                         "/home/zcq/VDB/data/coco_1k");
    std::string ds_name = DatasetName(data_dir);
    std::string ts = Timestamp();

    Log("=== VDB E2E Benchmark ===\n");
    Log("Dataset: %s\n", data_dir.c_str());

    // ================================================================
    // Phase A: Load Data
    // ================================================================
    Log("\n[Phase A] Loading data...\n");

    auto img_emb_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load image_embeddings: %s\n",
                     img_emb_or.status().ToString().c_str());
        return 1;
    }
    auto& img_emb = img_emb_or.value();

    auto img_ids_or = io::LoadNpyInt64(data_dir + "/image_ids.npy");
    if (!img_ids_or.ok()) {
        std::fprintf(stderr, "Failed to load image_ids: %s\n",
                     img_ids_or.status().ToString().c_str());
        return 1;
    }
    auto& img_ids = img_ids_or.value();

    auto qry_emb_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load query_embeddings: %s\n",
                     qry_emb_or.status().ToString().c_str());
        return 1;
    }
    auto& qry_emb = qry_emb_or.value();

    auto qry_ids_or = io::LoadNpyInt64(data_dir + "/query_ids.npy");
    if (!qry_ids_or.ok()) {
        std::fprintf(stderr, "Failed to load query_ids: %s\n",
                     qry_ids_or.status().ToString().c_str());
        return 1;
    }
    auto& qry_ids = qry_ids_or.value();

    uint32_t N = img_emb.rows;
    uint32_t Q_total = qry_emb.rows;
    Dim dim = static_cast<Dim>(img_emb.cols);

    int q_limit = GetIntArg(argc, argv, "--queries", 0);
    uint32_t Q = (q_limit > 0 && static_cast<uint32_t>(q_limit) < Q_total)
                     ? static_cast<uint32_t>(q_limit) : Q_total;

    Log("  Images: %u, Queries: %u/%u, Dim: %u\n", N, Q, Q_total, dim);

    // Load metadata
    std::unordered_map<int64_t, std::string> id_to_caption;
    auto s = io::ReadJsonlLines(data_dir + "/metadata.jsonl",
        [&](uint32_t, std::string_view line) {
            int64_t id = ParseImageId(line);
            if (id >= 0) {
                id_to_caption[id] = ParseCaption(line);
            }
        });
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to load metadata: %s\n", s.ToString().c_str());
        return 1;
    }
    Log("  Metadata entries: %zu\n", id_to_caption.size());

    // ================================================================
    // Phase B: Brute-Force Ground Truth
    // ================================================================
    const uint32_t GT_K = 10;
    Log("\n[Phase B] Computing brute-force ground truth (top-%u)...\n", GT_K);

    auto t_bf_start = std::chrono::steady_clock::now();

    std::vector<std::vector<int64_t>> gt_topk(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry_emb.data.data() + static_cast<size_t>(qi) * dim;

        // Compute distances to all images
        std::vector<std::pair<float, int64_t>> dists(N);
        for (uint32_t j = 0; j < N; ++j) {
            const float* ivec = img_emb.data.data() + static_cast<size_t>(j) * dim;
            dists[j] = {L2Sqr(qvec, ivec, dim), img_ids.data[j]};
        }

        // Partial sort for top-K
        std::partial_sort(dists.begin(), dists.begin() + GT_K, dists.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        gt_topk[qi].resize(GT_K);
        for (uint32_t k = 0; k < GT_K; ++k) {
            gt_topk[qi][k] = dists[k].second;
        }

        if ((qi + 1) % 100 == 0 || qi + 1 == Q) {
            double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_bf_start).count();
            Log("  GT progress: %u/%u (%.0f ms)\n", qi + 1, Q, elapsed);
        }
    }

    double brute_force_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_bf_start).count();
    Log("  Brute-force time: %.1f ms\n", brute_force_time_ms);

    // ================================================================
    // Phase C: Build Index
    // ================================================================
    std::string output_dir = "/home/zcq/VDB/test/" + ds_name + "_" + ts;
    std::string index_dir = output_dir + "/index";
    fs::create_directories(index_dir);

    Log("\n[Phase C] Building index → %s\n", index_dir.c_str());

    IvfBuilderConfig cfg;
    cfg.nlist = 32;
    cfg.max_iterations = 20;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = std::min(100u, N);
    cfg.calibration_topk = 10;
    cfg.page_size = 4096;
    cfg.payload_schemas = {
        {0, "id",      DType::INT64,  false},
        {1, "caption", DType::STRING, false},
    };

    PayloadFn payload_fn = [&](uint32_t idx) -> std::vector<Datum> {
        int64_t id = img_ids.data[idx];
        auto it = id_to_caption.find(id);
        std::string cap = (it != id_to_caption.end()) ? it->second : "";
        return {Datum::Int64(id), Datum::String(std::move(cap))};
    };

    IvfBuilder builder(cfg);
    builder.SetProgressCallback([](uint32_t cluster, uint32_t total) {
        if ((cluster + 1) % 8 == 0 || cluster + 1 == total) {
            Log("  Build progress: cluster %u/%u\n", cluster + 1, total);
        }
    });

    auto t_build_start = std::chrono::steady_clock::now();
    Log("  Starting Build...\n");
    s = builder.Build(img_emb.data.data(), N, dim, index_dir, payload_fn);
    double training_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_build_start).count();

    if (!s.ok()) {
        std::fprintf(stderr, "Build failed: %s\n", s.ToString().c_str());
        return 1;
    }
    Log("  Build time: %.1f ms\n", training_time_ms);

    // ================================================================
    // Phase D: Query
    // ================================================================
    Log("\n[Phase D] Querying with IoUring...\n");

    IvfIndex index;
    s = index.Open(index_dir);
    if (!s.ok()) {
        std::fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
        return 1;
    }

    IoUringReader reader;
    auto init_status = reader.Init(64, 4096);
    if (!init_status.ok()) {
        std::fprintf(stderr, "IoUring init failed: %s\n", init_status.ToString().c_str());
        return 1;
    }

    SearchConfig search_cfg;
    search_cfg.top_k = GT_K;
    search_cfg.nprobe = 32;
    search_cfg.early_stop = true;
    search_cfg.prefetch_depth = 16;
    search_cfg.io_queue_depth = 64;

    OverlapScheduler scheduler(index, reader, search_cfg);

    // Per-query results
    struct QueryResult {
        int64_t query_id;
        std::vector<int64_t> predicted_ids;
        std::vector<float> predicted_dists;
        double query_time_ms;
        double io_wait_ms;
        double probe_time_ms;
        bool early_stopped;
        uint32_t clusters_skipped;
        uint32_t total_probed;
        uint32_t safe_in;
        uint32_t safe_out;
        uint32_t uncertain;
    };

    std::vector<QueryResult> qresults(Q);

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry_emb.data.data() + static_cast<size_t>(qi) * dim;
        auto results = scheduler.Search(qvec);

        QueryResult& qr = qresults[qi];
        qr.query_id = qry_ids.data[qi];
        qr.query_time_ms = results.stats().total_time_ms;
        qr.io_wait_ms = results.stats().io_wait_time_ms;
        qr.probe_time_ms = results.stats().probe_time_ms;
        qr.early_stopped = results.stats().early_stopped;
        qr.clusters_skipped = results.stats().clusters_skipped;
        qr.total_probed = results.stats().total_probed;
        qr.safe_in = results.stats().total_safe_in;
        qr.safe_out = results.stats().total_safe_out;
        qr.uncertain = results.stats().total_uncertain;

        for (uint32_t j = 0; j < results.size(); ++j) {
            qr.predicted_dists.push_back(results[j].distance);
            if (!results[j].payload.empty() &&
                results[j].payload[0].dtype == DType::INT64) {
                qr.predicted_ids.push_back(results[j].payload[0].fixed.i64);
            }
        }

        if ((qi + 1) % 100 == 0 || qi + 1 == Q) {
            Log("  Query progress: %u/%u\n", qi + 1, Q);
        }
    }

    Log("  All %u queries complete.\n", Q);

    // ================================================================
    // Phase E: Evaluate
    // ================================================================
    Log("\n[Phase E] Computing metrics...\n");

    // Recall
    double recall_sum[3] = {0, 0, 0};
    uint32_t recall_K[3] = {1, 5, 10};
    for (uint32_t qi = 0; qi < Q; ++qi) {
        for (int r = 0; r < 3; ++r) {
            recall_sum[r] += ComputeRecallAtK(qresults[qi].predicted_ids,
                                               gt_topk[qi], recall_K[r]);
        }
    }
    double recall_at[3];
    for (int r = 0; r < 3; ++r) recall_at[r] = recall_sum[r] / Q;

    Log("  recall@1 = %.4f\n", recall_at[0]);
    Log("  recall@5 = %.4f\n", recall_at[1]);
    Log("  recall@10 = %.4f\n", recall_at[2]);

    // Timing percentiles
    std::vector<double> query_times(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) query_times[qi] = qresults[qi].query_time_ms;
    std::sort(query_times.begin(), query_times.end());

    double avg_query_ms = std::accumulate(query_times.begin(), query_times.end(), 0.0) / Q;
    double p50 = Percentile(query_times, 0.50);
    double p95 = Percentile(query_times, 0.95);
    double p99 = Percentile(query_times, 0.99);

    Log("  avg query: %.3f ms, p50: %.3f, p95: %.3f, p99: %.3f\n",
                avg_query_ms, p50, p95, p99);

    // Pipeline stats averages
    double sum_probed = 0, sum_si = 0, sum_so = 0, sum_unc = 0;
    double sum_io_wait = 0, sum_probe = 0, sum_total = 0;
    uint32_t early_count = 0;
    double sum_skipped = 0;
    for (uint32_t qi = 0; qi < Q; ++qi) {
        sum_probed += qresults[qi].total_probed;
        sum_si += qresults[qi].safe_in;
        sum_so += qresults[qi].safe_out;
        sum_unc += qresults[qi].uncertain;
        sum_io_wait += qresults[qi].io_wait_ms;
        sum_probe += qresults[qi].probe_time_ms;
        sum_total += qresults[qi].query_time_ms;
        if (qresults[qi].early_stopped) {
            early_count++;
            sum_skipped += qresults[qi].clusters_skipped;
        }
    }

    double avg_io_wait = sum_io_wait / Q;
    double avg_cpu = (sum_total - sum_io_wait) / Q;
    double avg_probe = sum_probe / Q;
    double early_pct = static_cast<double>(early_count) / Q;
    double avg_skipped = early_count > 0 ? sum_skipped / early_count : 0;

    Log("  avg io_wait: %.3f ms, avg cpu: %.3f ms, avg probe: %.3f ms\n",
                avg_io_wait, avg_cpu, avg_probe);
    Log("  early_stop: %.1f%%, avg skipped: %.1f clusters\n",
                early_pct * 100, avg_skipped);

    // ================================================================
    // Phase F: Output JSON
    // ================================================================
    Log("\n[Phase F] Writing results to %s\n", output_dir.c_str());

    // config.json
    {
        std::ofstream f(output_dir + "/config.json");
        f << "{\n";
        f << "  " << JStr("dataset", ds_name) << ",\n";
        f << "  " << JStr("dataset_path", data_dir) << ",\n";
        f << "  " << JStr("timestamp", ts) << ",\n";
        f << "  " << JInt("num_images", N) << ",\n";
        f << "  " << JInt("num_queries", Q) << ",\n";
        f << "  " << JInt("dimension", dim) << ",\n";
        f << "  \"build_config\": {\n";
        f << "    " << JInt("nlist", cfg.nlist) << ",\n";
        f << "    " << JInt("max_iterations", cfg.max_iterations) << ",\n";
        f << "    " << JInt("seed", static_cast<int64_t>(cfg.seed)) << ",\n";
        f << "    " << JInt("rabitq_bits", cfg.rabitq.bits) << ",\n";
        f << "    " << JInt("rabitq_block_size", cfg.rabitq.block_size) << ",\n";
        f << "    " << JNum("rabitq_c_factor", cfg.rabitq.c_factor) << ",\n";
        f << "    " << JInt("page_size", cfg.page_size) << "\n";
        f << "  },\n";
        f << "  \"search_config\": {\n";
        f << "    " << JInt("top_k", search_cfg.top_k) << ",\n";
        f << "    " << JInt("nprobe", search_cfg.nprobe) << ",\n";
        f << "    " << JBool("early_stop", search_cfg.early_stop) << ",\n";
        f << "    " << JInt("prefetch_depth", search_cfg.prefetch_depth) << ",\n";
        f << "    " << JInt("io_queue_depth", search_cfg.io_queue_depth) << "\n";
        f << "  }\n";
        f << "}\n";
    }

    // results.json
    {
        std::ofstream f(output_dir + "/results.json");
        f << "{\n";

        // metrics
        f << "  \"metrics\": {\n";
        f << "    " << JNum("training_time_ms", training_time_ms) << ",\n";
        f << "    " << JNum("brute_force_time_ms", brute_force_time_ms) << ",\n";
        f << "    " << JNum("recall_at_1", recall_at[0]) << ",\n";
        f << "    " << JNum("recall_at_5", recall_at[1]) << ",\n";
        f << "    " << JNum("recall_at_10", recall_at[2]) << ",\n";
        f << "    " << JNum("avg_query_time_ms", avg_query_ms) << ",\n";
        f << "    " << JNum("p50_query_time_ms", p50) << ",\n";
        f << "    " << JNum("p95_query_time_ms", p95) << ",\n";
        f << "    " << JNum("p99_query_time_ms", p99) << ",\n";
        f << "    " << JInt("num_queries", Q) << "\n";
        f << "  },\n";

        // pipeline_stats
        f << "  \"pipeline_stats\": {\n";
        f << "    " << JNum("avg_total_probed", sum_probed / Q) << ",\n";
        f << "    " << JNum("avg_safe_in", sum_si / Q) << ",\n";
        f << "    " << JNum("avg_safe_out", sum_so / Q) << ",\n";
        f << "    " << JNum("avg_uncertain", sum_unc / Q) << ",\n";
        f << "    " << JNum("avg_io_wait_ms", avg_io_wait) << ",\n";
        f << "    " << JNum("avg_cpu_time_ms", avg_cpu) << ",\n";
        f << "    " << JNum("avg_probe_time_ms", avg_probe) << ",\n";
        f << "    " << JNum("early_stopped_pct", early_pct) << ",\n";
        f << "    " << JNum("avg_clusters_skipped", avg_skipped) << "\n";
        f << "  },\n";

        // per_query_sample
        uint32_t stride = std::max(Q / 50, 1u);
        f << "  \"per_query_sample\": [\n";
        bool first = true;
        for (uint32_t qi = 0; qi < Q; qi += stride) {
            if (!first) f << ",\n";
            first = false;

            const auto& qr = qresults[qi];
            double r10 = ComputeRecallAtK(qr.predicted_ids, gt_topk[qi], 10);
            bool hit1 = !qr.predicted_ids.empty() && !gt_topk[qi].empty() &&
                        qr.predicted_ids[0] == gt_topk[qi][0];
            double cpu_ms = qr.query_time_ms - qr.io_wait_ms;

            f << "    {\n";
            f << "      " << JInt("query_id", qr.query_id) << ",\n";
            f << "      " << JArr64("gt_top10_ids", gt_topk[qi]) << ",\n";
            f << "      " << JArr64("predicted_top10_ids", qr.predicted_ids) << ",\n";
            f << "      " << JArrF("predicted_top10_distances", qr.predicted_dists) << ",\n";
            f << "      " << JBool("hit_at_1", hit1) << ",\n";
            f << "      " << JNum("recall_at_10", r10) << ",\n";
            f << "      " << JNum("query_time_ms", qr.query_time_ms) << ",\n";
            f << "      " << JNum("io_wait_ms", qr.io_wait_ms) << ",\n";
            f << "      " << JNum("cpu_time_ms", cpu_ms) << ",\n";
            f << "      " << JBool("early_stopped", qr.early_stopped) << ",\n";
            f << "      " << JInt("clusters_skipped", qr.clusters_skipped) << "\n";
            f << "    }";
        }
        f << "\n  ]\n";
        f << "}\n";
    }

    // ================================================================
    // Summary
    // ================================================================
    Log("\n=== Summary ===\n");
    Log("  recall@1=%.4f  recall@5=%.4f  recall@10=%.4f\n",
                recall_at[0], recall_at[1], recall_at[2]);
    Log("  avg_query=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n",
                avg_query_ms, p50, p95, p99);
    Log("  build_time=%.1f ms  brute_force=%.1f ms\n",
                training_time_ms, brute_force_time_ms);
    Log("  io_wait=%.3f ms  cpu=%.3f ms  probe=%.3f ms\n",
                avg_io_wait, avg_cpu, avg_probe);
    Log("  Output: %s\n", output_dir.c_str());

    return 0;
}
