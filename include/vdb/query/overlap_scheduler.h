#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/buffer_pool.h"
#include "vdb/query/search_context.h"
#include "vdb/query/search_results.h"

namespace vdb {
namespace query {

/// Single-threaded two-phase query scheduler.
///
/// Phase 1 (ProbeAndSubmit): For each probed cluster, RaBitQ estimate →
///   ConANN classify → PrepRead (VEC_ONLY / ALL / skip) → Submit.
/// Phase 2 (DrainAndRerank): WaitAndPoll completions → InferType →
///   ConsumeVec / ConsumeAll / ConsumePayload → L2Sqr rerank.
/// Phase 3 (FetchMissingPayloads): Uncertain records in TopK without
///   cached payload → batch PrepRead + drain.
/// Phase 4 (AssembleResults): ParsePayload → SearchResults.
class OverlapScheduler {
 public:
    OverlapScheduler(index::IvfIndex& index, AsyncReader& reader,
                     const SearchConfig& config);
    ~OverlapScheduler();

    /// Execute a single query and return results.
    SearchResults Search(const float* query_vec);

 private:
    struct PendingRead {
        AddressEntry addr;
        uint64_t read_offset;
        uint32_t read_length;
    };

    void ProbeAndSubmit(SearchContext& ctx, class RerankConsumer& reranker);
    void DrainAndRerank(SearchContext& ctx, class RerankConsumer& reranker);
    void FetchMissingPayloads(SearchContext& ctx,
                              class RerankConsumer& reranker,
                              const std::vector<CollectorEntry>& results);
    SearchResults AssembleResults(class RerankConsumer& reranker,
                                  const std::vector<CollectorEntry>& results);

    ReadTaskType InferType(const PendingRead& pr) const;

    index::IvfIndex& index_;
    AsyncReader& reader_;
    SearchConfig config_;
    BufferPool buffer_pool_;
    std::unordered_map<uint8_t*, PendingRead> pending_;

    uint32_t vec_bytes_;
    uint32_t num_words_;
};

}  // namespace query
}  // namespace vdb
