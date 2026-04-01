#pragma once

#include <cstdint>

#include "vdb/common/types.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/query/result_collector.h"

namespace vdb {
namespace query {

struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 8;
    uint32_t probe_batch_size = 128;
    uint32_t cluster_atomic_threshold = 1024;
    uint32_t io_queue_depth = 64;
    uint32_t cq_entries = 4096;
    uint32_t safein_all_threshold = 256 * 1024;  // 256KB

    // Early stop: skip remaining clusters when TopK quality exceeds d_k
    bool early_stop = true;

    // Phase 8: Async cluster prefetch
    uint32_t prefetch_depth = 16;      // Initial cluster block prefetch count
    uint32_t initial_prefetch = 4;     // CRC mode: initial cluster prefetch count
    uint32_t refill_threshold = 2;     // Refill when inflight_clusters drops below
    uint32_t refill_count = 2;         // Number of clusters to refill per check

    // CRC early stop parameters (nullptr = use legacy d_k early stop)
    const index::CalibrationResults* crc_params = nullptr;
};

struct SearchStats {
    uint32_t total_probed = 0;
    uint32_t total_safe_in = 0;
    uint32_t total_safe_out = 0;
    uint32_t total_uncertain = 0;
    uint32_t total_io_submitted = 0;
    uint32_t total_reranked = 0;
    uint32_t total_payload_prefetched = 0;
    uint32_t total_payload_fetched = 0;
    bool early_stopped = false;
    uint32_t clusters_skipped = 0;
    uint32_t crc_clusters_probed = 0;
    double probe_time_ms = 0;
    double rerank_time_ms = 0;
    double total_time_ms = 0;
    double io_wait_time_ms = 0;
    // Stage 2 (ExRaBitQ re-classification, only when bits > 1)
    uint32_t s2_safe_in = 0;
    uint32_t s2_safe_out = 0;
    uint32_t s2_uncertain = 0;
};

class SearchContext {
 public:
    SearchContext(const float* query_vec, const SearchConfig& config)
        : query_vec_(query_vec), config_(config),
          collector_(config.top_k) {}

    const float* query_vec() const { return query_vec_; }
    const SearchConfig& config() const { return config_; }
    SearchStats& stats() { return stats_; }
    const SearchStats& stats() const { return stats_; }
    ResultCollector& collector() { return collector_; }

 private:
    const float* query_vec_;
    SearchConfig config_;
    SearchStats stats_;
    ResultCollector collector_;
};

}  // namespace query
}  // namespace vdb
