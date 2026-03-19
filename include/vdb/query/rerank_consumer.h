#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "vdb/common/types.h"
#include "vdb/query/result_collector.h"
#include "vdb/query/search_context.h"

namespace vdb {
namespace query {

/// Consumes I/O completion buffers and performs L2Sqr reranking.
///
/// Three consumption modes based on ReadTaskType:
///   - ConsumeVec:     VEC_ONLY read → L2Sqr → TryInsert. Caller frees buf.
///   - ConsumeAll:     ALL read → L2Sqr → TryInsert → if TopK, cache payload.
///                     Caller frees buf.
///   - ConsumePayload: PAYLOAD read → transfer buf ownership to cache.
///
/// Payload cache is keyed by AddressEntry.offset (unique per record).
class RerankConsumer {
 public:
    /// @param ctx   SearchContext (provides query_vec and collector)
    /// @param dim   Vector dimensionality
    RerankConsumer(SearchContext& ctx, Dim dim);
    ~RerankConsumer();

    /// Consume a VEC_ONLY buffer: extract float vector, L2Sqr, TryInsert.
    /// Caller is responsible for freeing buf afterward.
    void ConsumeVec(uint8_t* buf, AddressEntry addr);

    /// Consume an ALL buffer: extract vector + payload.
    /// L2Sqr → TryInsert. If entered TopK, copies payload portion to cache.
    /// Caller is responsible for freeing buf afterward.
    void ConsumeAll(uint8_t* buf, AddressEntry addr);

    /// Consume a PAYLOAD buffer: transfers ownership to the payload cache.
    /// Caller must NOT free buf after this call.
    void ConsumePayload(uint8_t* buf, AddressEntry addr);

    /// Check if payload is cached for this address offset.
    bool HasPayload(uint64_t offset) const;

    /// Take ownership of a cached payload buffer (removes from cache).
    /// Returns nullptr if not found.
    std::unique_ptr<uint8_t[]> TakePayload(uint64_t offset);

    /// Cache a payload buffer (takes ownership).
    void CachePayload(uint64_t offset, std::unique_ptr<uint8_t[]> buf);

    /// Remove cached payloads not in the final TopK.
    /// Call after Finalize to free memory for entries that didn't make it.
    void CleanupUnusedCache(const std::vector<CollectorEntry>& final_results);

 private:
    SearchContext& ctx_;
    Dim dim_;
    uint32_t vec_bytes_;  // dim * sizeof(float)

    // Payload cache: addr.offset → owned payload buffer
    std::unordered_map<uint64_t, std::unique_ptr<uint8_t[]>> payload_cache_;
};

}  // namespace query
}  // namespace vdb
