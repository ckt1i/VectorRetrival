/// bench_e2e.cpp — End-to-end benchmark on real COCO datasets.
///
/// Measures: recall@{1,5,10}, query latency (avg/p50/p95/p99),
///           build time, brute-force GT time, IO/CPU pipeline utilization.
///
/// Usage:
///   bench_e2e [--dataset /path/to/coco_1k]
///             [--crc 1] [--crc-alpha 0.1] [--crc-calib 0.5] [--crc-tune 0.1]
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
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/jsonl_reader.h"
#include "vdb/io/npy_reader.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/simd/distance_l2.h"

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

static float GetFloatArg(int argc, char* argv[], const char* name, float default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::strtof(argv[i + 1], nullptr);
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
// Log
// ============================================================================

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// Per-query result struct (used by both rounds)
// ============================================================================

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

// ============================================================================
// Aggregated round metrics
// ============================================================================

struct RoundMetrics {
    double recall_at[3] = {};          // recall@1, @5, @10
    double avg_query_ms = 0;
    double p50 = 0, p95 = 0, p99 = 0;
    double avg_io_wait = 0;
    double avg_cpu = 0;
    double avg_probe = 0;
    double avg_probed = 0;
    double avg_safe_in = 0;
    double avg_safe_out = 0;
    double avg_uncertain = 0;
    double early_pct = 0;
    double avg_skipped = 0;
    double overlap_ratio = 0;
};

// ============================================================================
// Run one query round and compute metrics
// ============================================================================

static std::pair<std::vector<QueryResult>, RoundMetrics> RunQueryRound(
    const char* label,
    IvfIndex& index, IoUringReader& reader,
    const SearchConfig& search_cfg,
    const float* queries, uint32_t Q, Dim dim,
    const std::vector<int64_t>& qry_ids_data,
    const std::vector<std::vector<int64_t>>& gt_topk) {

    Log("\n[%s] Querying %u vectors...\n", label, Q);

    OverlapScheduler scheduler(index, reader, search_cfg);

    std::vector<QueryResult> qresults(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = queries + static_cast<size_t>(qi) * dim;
        auto results = scheduler.Search(qvec);

        QueryResult& qr = qresults[qi];
        qr.query_id = qry_ids_data[qi];
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
            Log("  %s progress: %u/%u\n", label, qi + 1, Q);
        }
    }
    Log("  %s: all %u queries complete.\n", label, Q);

    // Compute metrics
    RoundMetrics m;
    uint32_t recall_K[3] = {1, 5, 10};
    double recall_sum[3] = {0, 0, 0};
    for (uint32_t qi = 0; qi < Q; ++qi) {
        for (int r = 0; r < 3; ++r) {
            recall_sum[r] += ComputeRecallAtK(qresults[qi].predicted_ids,
                                               gt_topk[qi], recall_K[r]);
        }
    }
    for (int r = 0; r < 3; ++r) m.recall_at[r] = recall_sum[r] / Q;

    std::vector<double> query_times(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) query_times[qi] = qresults[qi].query_time_ms;
    std::sort(query_times.begin(), query_times.end());
    m.avg_query_ms = std::accumulate(query_times.begin(), query_times.end(), 0.0) / Q;
    m.p50 = Percentile(query_times, 0.50);
    m.p95 = Percentile(query_times, 0.95);
    m.p99 = Percentile(query_times, 0.99);

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

    m.avg_io_wait = sum_io_wait / Q;
    m.avg_cpu = (sum_total - sum_io_wait) / Q;
    m.avg_probe = sum_probe / Q;
    m.avg_probed = sum_probed / Q;
    m.avg_safe_in = sum_si / Q;
    m.avg_safe_out = sum_so / Q;
    m.avg_uncertain = sum_unc / Q;
    m.early_pct = static_cast<double>(early_count) / Q;
    m.avg_skipped = early_count > 0 ? sum_skipped / early_count : 0;
    m.overlap_ratio = (sum_total > 0) ? 1.0 - sum_io_wait / sum_total : 0.0;

    Log("  %s: recall@1=%.4f @5=%.4f @10=%.4f\n", label,
        m.recall_at[0], m.recall_at[1], m.recall_at[2]);
    Log("  %s: avg=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n", label,
        m.avg_query_ms, m.p50, m.p95, m.p99);
    Log("  %s: io_wait=%.3f ms  cpu=%.3f ms  probe=%.3f ms\n", label,
        m.avg_io_wait, m.avg_cpu, m.avg_probe);
    Log("  %s: safe_in=%.1f  safe_out=%.1f  uncertain=%.1f\n", label,
        m.avg_safe_in, m.avg_safe_out, m.avg_uncertain);
    Log("  %s: early_stop=%.1f%%  avg_skipped=%.1f  overlap=%.4f\n", label,
        m.early_pct * 100, m.avg_skipped, m.overlap_ratio);

    return {std::move(qresults), m};
}

// ============================================================================
// Write round metrics to JSON
// ============================================================================

static void WriteRoundMetricsJson(std::ofstream& f, const std::string& prefix,
                                   const RoundMetrics& m) {
    f << "    \"" << prefix << "\": {\n";
    f << "      " << JNum("recall_at_1", m.recall_at[0]) << ",\n";
    f << "      " << JNum("recall_at_5", m.recall_at[1]) << ",\n";
    f << "      " << JNum("recall_at_10", m.recall_at[2]) << ",\n";
    f << "      " << JNum("avg_query_time_ms", m.avg_query_ms) << ",\n";
    f << "      " << JNum("p50_query_time_ms", m.p50) << ",\n";
    f << "      " << JNum("p95_query_time_ms", m.p95) << ",\n";
    f << "      " << JNum("p99_query_time_ms", m.p99) << ",\n";
    f << "      " << JNum("avg_io_wait_ms", m.avg_io_wait) << ",\n";
    f << "      " << JNum("avg_cpu_time_ms", m.avg_cpu) << ",\n";
    f << "      " << JNum("avg_probe_time_ms", m.avg_probe) << ",\n";
    f << "      " << JNum("avg_total_probed", m.avg_probed) << ",\n";
    f << "      " << JNum("avg_safe_in", m.avg_safe_in) << ",\n";
    f << "      " << JNum("avg_safe_out", m.avg_safe_out) << ",\n";
    f << "      " << JNum("avg_uncertain", m.avg_uncertain) << ",\n";
    f << "      " << JNum("early_stopped_pct", m.early_pct) << ",\n";
    f << "      " << JNum("avg_clusters_skipped", m.avg_skipped) << ",\n";
    f << "      " << JNum("overlap_ratio", m.overlap_ratio) << "\n";
    f << "    }";
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetStringArg(argc, argv, "--dataset",
                                         "/home/zcq/VDB/data/coco_1k");
    std::string output_base = GetStringArg(argc, argv, "--output",
                                            "/home/zcq/VDB/test/");
    int arg_nlist      = GetIntArg(argc, argv, "--nlist", 32);
    int arg_nprobe     = GetIntArg(argc, argv, "--nprobe", 32);
    int arg_topk       = GetIntArg(argc, argv, "--topk", 10);
    int arg_early_stop = GetIntArg(argc, argv, "--early-stop", 1);
    int arg_bits       = GetIntArg(argc, argv, "--bits", 1);
    int arg_block_size = GetIntArg(argc, argv, "--block-size", 64);
    float arg_c_factor = GetFloatArg(argc, argv, "--c-factor", 5.75f);
    int arg_max_iter   = GetIntArg(argc, argv, "--max-iter", 20);
    int arg_seed       = GetIntArg(argc, argv, "--seed", 42);
    int arg_page_size  = GetIntArg(argc, argv, "--page-size", 4096);
    int arg_p_for_dk   = GetIntArg(argc, argv, "--p-for-dk", 99);

    // CRC comparison mode
    int arg_crc         = GetIntArg(argc, argv, "--crc", 0);
    float arg_crc_alpha = GetFloatArg(argc, argv, "--crc-alpha", 0.1f);
    float arg_crc_calib = GetFloatArg(argc, argv, "--crc-calib", 0.5f);
    float arg_crc_tune  = GetFloatArg(argc, argv, "--crc-tune", 0.1f);

    std::string ds_name = DatasetName(data_dir);
    std::string ts = Timestamp();

    Log("=== VDB E2E Benchmark ===\n");
    Log("Dataset: %s\n", data_dir.c_str());
    if (arg_crc) Log("CRC comparison mode: ON (alpha=%.2f)\n", arg_crc_alpha);

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
    const uint32_t GT_K = static_cast<uint32_t>(arg_topk);
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
    std::string output_dir = output_base + "/" + ds_name + "_" + ts;
    std::string index_dir = output_dir + "/index";
    fs::create_directories(index_dir);

    Log("\n[Phase C] Building index -> %s\n", index_dir.c_str());

    IvfBuilderConfig cfg;
    cfg.nlist = static_cast<uint32_t>(arg_nlist);
    cfg.max_iterations = static_cast<uint32_t>(arg_max_iter);
    cfg.seed = static_cast<uint64_t>(arg_seed);
    cfg.rabitq = {static_cast<uint8_t>(arg_bits),
                  static_cast<uint32_t>(arg_block_size), arg_c_factor};
    cfg.calibration_samples = std::min(100u, N);
    cfg.calibration_topk = GT_K;
    cfg.calibration_percentile = static_cast<float>(arg_p_for_dk) / 100.0f;
    cfg.page_size = static_cast<uint32_t>(arg_page_size);
    cfg.calibration_queries = qry_emb.data.data();
    cfg.num_calibration_queries = Q;
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
    // Phase C.5: Open Index + CRC Calibration (from disk)
    // ================================================================
    IvfIndex index;
    s = index.Open(index_dir);
    if (!s.ok()) {
        std::fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
        return 1;
    }

    CalibrationResults calib_results;

    if (arg_crc) {
        Log("\n[Phase C.5] CRC calibration from disk...\n");
        auto t_crc_start = std::chrono::steady_clock::now();

        auto& seg = index.segment();
        uint32_t nlist = index.nlist();

        // Load all cluster data from disk
        // vectors: for GT computation
        // codes: for CRC calibration (via codes_block pointer)
        std::vector<float> all_vectors;
        std::vector<uint32_t> all_ids;
        all_vectors.reserve(static_cast<size_t>(N) * dim);
        all_ids.reserve(N);

        // Per-cluster codes_block pointers (valid after EnsureClusterLoaded)
        struct PerClusterInfo {
            const uint8_t* codes_block = nullptr;
            uint32_t count = 0;
            uint32_t global_start = 0;  // start index in all_vectors
        };
        std::vector<PerClusterInfo> cluster_infos(nlist);

        // code_entry_size = num_words * 8 + sizeof(float) + sizeof(uint32_t)
        uint32_t num_words = (static_cast<uint32_t>(dim) + 63) / 64;
        uint32_t code_entry_size = num_words * sizeof(uint64_t)
                                   + sizeof(float) + sizeof(uint32_t);
        uint32_t global_offset = 0;

        for (uint32_t cid = 0; cid < nlist; ++cid) {
            s = seg.EnsureClusterLoaded(cid);
            if (!s.ok()) {
                Log("  Warning: failed to load cluster %u: %s\n",
                    cid, s.ToString().c_str());
                continue;
            }

            uint32_t count = seg.GetNumRecords(cid);
            cluster_infos[cid].codes_block = seg.GetCodePtr(cid, 0);
            cluster_infos[cid].count = count;
            cluster_infos[cid].global_start = global_offset;

            // Read raw vectors from .dat
            all_vectors.resize(static_cast<size_t>(global_offset + count) * dim);
            for (uint32_t i = 0; i < count; ++i) {
                AddressEntry addr = seg.GetAddress(cid, i);
                s = seg.ReadVector(addr,
                    all_vectors.data() + static_cast<size_t>(global_offset + i) * dim);
                if (!s.ok()) {
                    Log("  Warning: failed to read vector cid=%u i=%u: %s\n",
                        cid, i, s.ToString().c_str());
                }
                all_ids.push_back(global_offset + i);
            }

            global_offset += count;
        }

        uint32_t total_vecs = global_offset;
        Log("  Loaded %u vectors from %u clusters (code_entry_size=%u)\n",
            total_vecs, nlist, code_entry_size);

        // Compute brute-force GT for CRC calibrator (uint32_t IDs)
        Log("  Computing CRC ground truth...\n");
        std::vector<std::vector<uint32_t>> crc_gt(Q);
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* qvec = qry_emb.data.data() + static_cast<size_t>(qi) * dim;

            std::vector<std::pair<float, uint32_t>> dists(total_vecs);
            for (uint32_t j = 0; j < total_vecs; ++j) {
                dists[j] = {simd::L2Sqr(qvec,
                    all_vectors.data() + static_cast<size_t>(j) * dim, dim), j};
            }

            uint32_t gt_k = std::min(GT_K, total_vecs);
            std::partial_sort(dists.begin(), dists.begin() + gt_k, dists.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            crc_gt[qi].resize(gt_k);
            for (uint32_t k = 0; k < gt_k; ++k) {
                crc_gt[qi][k] = dists[k].second;
            }
        }

        // Build ClusterData array for CrcCalibrator
        std::vector<ClusterData> clusters(nlist);
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            auto& ci = cluster_infos[cid];
            clusters[cid].vectors = all_vectors.data() +
                static_cast<size_t>(ci.global_start) * dim;
            clusters[cid].ids = all_ids.data() + ci.global_start;
            clusters[cid].count = ci.count;
            clusters[cid].codes_block = ci.codes_block;
            clusters[cid].code_entry_size = code_entry_size;
        }

        // Collect centroids into contiguous array
        std::vector<float> centroids_flat(static_cast<size_t>(nlist) * dim);
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            std::memcpy(centroids_flat.data() + static_cast<size_t>(cid) * dim,
                        index.centroid(cid), dim * sizeof(float));
        }

        // Run CRC calibration with RaBitQ distances
        CrcCalibrator::Config crc_cfg;
        crc_cfg.alpha = arg_crc_alpha;
        crc_cfg.top_k = GT_K;
        crc_cfg.calib_ratio = arg_crc_calib;
        crc_cfg.tune_ratio = arg_crc_tune;
        crc_cfg.seed = static_cast<uint64_t>(arg_seed);

        auto [cal, eval] = CrcCalibrator::CalibrateWithRaBitQ(
            crc_cfg, qry_emb.data.data(), Q, dim,
            centroids_flat.data(), nlist, clusters, crc_gt,
            index.rotation());
        calib_results = cal;

        double crc_time_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_crc_start).count();

        Log("  CRC calibration done (%.1f ms)\n", crc_time_ms);
        Log("  d_min=%.6f  d_max=%.6f  lamhat=%.6f\n",
            cal.d_min, cal.d_max, cal.lamhat);
        Log("  kreg=%u  reg_lambda=%.6f\n", cal.kreg, cal.reg_lambda);
        Log("  eval: FNR=%.4f  avg_probed=%.1f  test_size=%u\n",
            eval.actual_fnr, eval.avg_probed, eval.test_size);
        Log("  eval: recall@1=%.4f  @5=%.4f  @10=%.4f\n",
            eval.recall_at_1, eval.recall_at_5, eval.recall_at_10);
    }

    // ================================================================
    // Phase D: Query
    // ================================================================
    IoUringReader reader;
    auto init_status = reader.Init(64, 4096);
    if (!init_status.ok()) {
        std::fprintf(stderr, "IoUring init failed: %s\n", init_status.ToString().c_str());
        return 1;
    }

    // Prepare query IDs as a flat vector for RunQueryRound
    std::vector<int64_t> qry_ids_vec(qry_ids.data.begin(),
                                      qry_ids.data.begin() + Q);

    // Round 1: Baseline (d_k early stop, static SafeOut)
    SearchConfig baseline_cfg;
    baseline_cfg.top_k = GT_K;
    baseline_cfg.nprobe = static_cast<uint32_t>(arg_nprobe);
    baseline_cfg.early_stop = (arg_early_stop != 0);
    baseline_cfg.prefetch_depth = 16;
    baseline_cfg.io_queue_depth = 64;
    baseline_cfg.crc_params = nullptr;

    auto [qresults, baseline_m] = RunQueryRound(
        "Baseline", index, reader, baseline_cfg,
        qry_emb.data.data(), Q, dim, qry_ids_vec, gt_topk);

    // Round 2: CRC (CRC early stop + dynamic SafeOut)
    std::vector<QueryResult> crc_qresults;
    RoundMetrics crc_m;

    if (arg_crc) {
        SearchConfig crc_search_cfg = baseline_cfg;
        crc_search_cfg.crc_params = &calib_results;
        crc_search_cfg.initial_prefetch = 4;

        auto [crc_qr, cm] = RunQueryRound(
            "CRC", index, reader, crc_search_cfg,
            qry_emb.data.data(), Q, dim, qry_ids_vec, gt_topk);
        crc_qresults = std::move(crc_qr);
        crc_m = cm;
    }

    // ================================================================
    // Phase E: Comparison Output
    // ================================================================
    if (arg_crc) {
        Log("\n=== Baseline vs CRC Comparison ===\n");
        Log("  %-22s %10s %10s\n", "Metric", "Baseline", "CRC");
        Log("  %-22s %10s %10s\n", "----------------------", "----------", "----------");
        Log("  %-22s %10.4f %10.4f\n", "recall@1",   baseline_m.recall_at[0], crc_m.recall_at[0]);
        Log("  %-22s %10.4f %10.4f\n", "recall@5",   baseline_m.recall_at[1], crc_m.recall_at[1]);
        Log("  %-22s %10.4f %10.4f\n", "recall@10",  baseline_m.recall_at[2], crc_m.recall_at[2]);
        Log("  %-22s %10.3f %10.3f\n", "avg_query_ms",   baseline_m.avg_query_ms, crc_m.avg_query_ms);
        Log("  %-22s %10.3f %10.3f\n", "p50_ms",         baseline_m.p50, crc_m.p50);
        Log("  %-22s %10.3f %10.3f\n", "p95_ms",         baseline_m.p95, crc_m.p95);
        Log("  %-22s %10.3f %10.3f\n", "p99_ms",         baseline_m.p99, crc_m.p99);
        Log("  %-22s %10.3f %10.3f\n", "avg_io_wait_ms", baseline_m.avg_io_wait, crc_m.avg_io_wait);
        Log("  %-22s %10.3f %10.3f\n", "avg_probe_ms",   baseline_m.avg_probe, crc_m.avg_probe);
        Log("  %-22s %10.4f %10.4f\n", "overlap_ratio",  baseline_m.overlap_ratio, crc_m.overlap_ratio);
        Log("  %-22s %10.1f %10.1f\n", "avg_probed",     baseline_m.avg_probed, crc_m.avg_probed);
        Log("  %-22s %10.1f %10.1f\n", "avg_safe_out",   baseline_m.avg_safe_out, crc_m.avg_safe_out);
        Log("  %-22s %10.1f %10.1f\n", "avg_safe_in",    baseline_m.avg_safe_in, crc_m.avg_safe_in);
        Log("  %-22s %10.1f %10.1f\n", "avg_uncertain",  baseline_m.avg_uncertain, crc_m.avg_uncertain);
        Log("  %-22s %10.1f%% %9.1f%%\n", "early_stop_rate",
            baseline_m.early_pct * 100, crc_m.early_pct * 100);
        Log("  %-22s %10.1f %10.1f\n", "avg_skipped",    baseline_m.avg_skipped, crc_m.avg_skipped);
    } else {
        Log("\n=== Summary ===\n");
        Log("  recall@1=%.4f  recall@5=%.4f  recall@10=%.4f\n",
            baseline_m.recall_at[0], baseline_m.recall_at[1], baseline_m.recall_at[2]);
        Log("  avg_query=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n",
            baseline_m.avg_query_ms, baseline_m.p50, baseline_m.p95, baseline_m.p99);
        Log("  build_time=%.1f ms  brute_force=%.1f ms\n",
            training_time_ms, brute_force_time_ms);
        Log("  io_wait=%.3f ms  cpu=%.3f ms  probe=%.3f ms\n",
            baseline_m.avg_io_wait, baseline_m.avg_cpu, baseline_m.avg_probe);
    }

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
        f << "    " << JInt("top_k", baseline_cfg.top_k) << ",\n";
        f << "    " << JInt("nprobe", baseline_cfg.nprobe) << ",\n";
        f << "    " << JBool("early_stop", baseline_cfg.early_stop) << ",\n";
        f << "    " << JInt("prefetch_depth", baseline_cfg.prefetch_depth) << ",\n";
        f << "    " << JInt("io_queue_depth", baseline_cfg.io_queue_depth) << "\n";
        f << "  }";
        if (arg_crc) {
            f << ",\n  \"crc_config\": {\n";
            f << "    " << JNum("alpha", arg_crc_alpha) << ",\n";
            f << "    " << JNum("calib_ratio", arg_crc_calib) << ",\n";
            f << "    " << JNum("tune_ratio", arg_crc_tune) << "\n";
            f << "  }";
        }
        f << "\n}\n";
    }

    // results.json
    {
        std::ofstream f(output_dir + "/results.json");
        f << "{\n";

        // metrics (baseline)
        f << "  \"metrics\": {\n";
        f << "    " << JNum("training_time_ms", training_time_ms) << ",\n";
        f << "    " << JNum("brute_force_time_ms", brute_force_time_ms) << ",\n";
        f << "    " << JNum("recall_at_1", baseline_m.recall_at[0]) << ",\n";
        f << "    " << JNum("recall_at_5", baseline_m.recall_at[1]) << ",\n";
        f << "    " << JNum("recall_at_10", baseline_m.recall_at[2]) << ",\n";
        f << "    " << JNum("avg_query_time_ms", baseline_m.avg_query_ms) << ",\n";
        f << "    " << JNum("p50_query_time_ms", baseline_m.p50) << ",\n";
        f << "    " << JNum("p95_query_time_ms", baseline_m.p95) << ",\n";
        f << "    " << JNum("p99_query_time_ms", baseline_m.p99) << ",\n";
        f << "    " << JInt("num_queries", Q) << "\n";
        f << "  },\n";

        // pipeline_stats (baseline)
        f << "  \"pipeline_stats\": {\n";
        f << "    " << JNum("avg_total_probed", baseline_m.avg_probed) << ",\n";
        f << "    " << JNum("avg_safe_in", baseline_m.avg_safe_in) << ",\n";
        f << "    " << JNum("avg_safe_out", baseline_m.avg_safe_out) << ",\n";
        f << "    " << JNum("avg_uncertain", baseline_m.avg_uncertain) << ",\n";
        f << "    " << JNum("avg_io_wait_ms", baseline_m.avg_io_wait) << ",\n";
        f << "    " << JNum("avg_cpu_time_ms", baseline_m.avg_cpu) << ",\n";
        f << "    " << JNum("avg_probe_time_ms", baseline_m.avg_probe) << ",\n";
        f << "    " << JNum("early_stopped_pct", baseline_m.early_pct) << ",\n";
        f << "    " << JNum("avg_clusters_skipped", baseline_m.avg_skipped) << ",\n";
        f << "    " << JNum("overlap_ratio", baseline_m.overlap_ratio) << "\n";
        f << "  },\n";

        // per_query_sample (baseline)
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
        f << "\n  ]";

        // CRC comparison section
        if (arg_crc) {
            f << ",\n  \"crc_comparison\": {\n";
            WriteRoundMetricsJson(f, "baseline", baseline_m);
            f << ",\n";
            WriteRoundMetricsJson(f, "crc", crc_m);
            f << ",\n";
            f << "    \"calibration\": {\n";
            f << "      " << JNum("d_min", calib_results.d_min) << ",\n";
            f << "      " << JNum("d_max", calib_results.d_max) << ",\n";
            f << "      " << JNum("lamhat", calib_results.lamhat) << ",\n";
            f << "      " << JInt("kreg", calib_results.kreg) << ",\n";
            f << "      " << JNum("reg_lambda", calib_results.reg_lambda) << "\n";
            f << "    }\n";
            f << "  }";
        }

        f << "\n}\n";
    }

    Log("  Output: %s\n", output_dir.c_str());

    return 0;
}
