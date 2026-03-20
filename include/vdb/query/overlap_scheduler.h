#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/buffer_pool.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/query/search_context.h"
#include "vdb/query/search_results.h"

namespace vdb {
namespace query {

/// Single-threaded query scheduler with async cluster prefetch.
///
/// Pipeline:
///   PrefetchClusters (sliding window io_uring batch) →
///   ProbeAndDrainInterleaved (unified event loop: probe CPU + rerank CQE) →
///   FinalDrain → FetchMissingPayloads → AssembleResults.
class OverlapScheduler {
 public:
    OverlapScheduler(index::IvfIndex& index, AsyncReader& reader,
                     const SearchConfig& config);
    ~OverlapScheduler();

    /// Execute a single query and return results.
    SearchResults Search(const float* query_vec);

 private:
    struct PendingIO {
        enum class Type : uint8_t {
            CLUSTER_BLOCK, VEC_ONLY, VEC_ALL, PAYLOAD
        };
        Type type;
        uint32_t cluster_id = 0;   // CLUSTER_BLOCK
        uint32_t block_size = 0;   // CLUSTER_BLOCK
        AddressEntry addr;          // VEC_ONLY / VEC_ALL / PAYLOAD
        uint64_t read_offset = 0;
        uint32_t read_length = 0;
    };

    void PrefetchClusters(SearchContext& ctx,
                          const std::vector<ClusterID>& sorted_clusters);
    void ProbeAndDrainInterleaved(SearchContext& ctx,
                                   class RerankConsumer& reranker,
                                   const std::vector<ClusterID>& sorted_clusters);
    void FinalDrain(SearchContext& ctx, class RerankConsumer& reranker);
    void DispatchCompletion(uint8_t* buf, SearchContext& ctx,
                            class RerankConsumer& reranker);
    void SubmitClusterRead(uint32_t cluster_id);
    void ProbeCluster(const ParsedCluster& pc, uint32_t cluster_id,
                      SearchContext& ctx, class RerankConsumer& reranker);
    void FetchMissingPayloads(SearchContext& ctx,
                              class RerankConsumer& reranker,
                              const std::vector<CollectorEntry>& results);
    SearchResults AssembleResults(class RerankConsumer& reranker,
                                  const std::vector<CollectorEntry>& results);

    index::IvfIndex& index_;
    AsyncReader& reader_;
    SearchConfig config_;
    BufferPool buffer_pool_;
    std::unordered_map<uint8_t*, PendingIO> pending_;

    // Sliding window state (reset per Search() call)
    std::unordered_map<uint32_t, ParsedCluster> ready_clusters_;
    uint32_t next_to_submit_ = 0;
    uint32_t inflight_clusters_ = 0;

    uint32_t vec_bytes_;
    uint32_t num_words_;
};

}  // namespace query
}  // namespace vdb
