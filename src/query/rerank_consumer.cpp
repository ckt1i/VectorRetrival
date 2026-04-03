#include "vdb/query/rerank_consumer.h"

#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "vdb/common/distance.h"

namespace vdb {
namespace query {

RerankConsumer::RerankConsumer(SearchContext& ctx, Dim dim)
    : ctx_(ctx), dim_(dim), vec_bytes_(dim * sizeof(float)) {}

RerankConsumer::~RerankConsumer() = default;

void RerankConsumer::ConsumeVec(uint8_t* buf, AddressEntry addr) {
    const float* vec = reinterpret_cast<const float*>(buf);
    float dist = L2Sqr(ctx_.query_vec(), vec, dim_);
    ctx_.collector().TryInsert(dist, addr);
    ctx_.stats().total_reranked++;
}

void RerankConsumer::ConsumeAll(uint8_t* buf, AddressEntry addr) {
    const float* vec = reinterpret_cast<const float*>(buf);
    float dist = L2Sqr(ctx_.query_vec(), vec, dim_);
    bool entered = ctx_.collector().TryInsert(dist, addr);

    if (entered && addr.size > vec_bytes_) {
        // Copy payload portion to cache (aligned for consistency)
        uint32_t payload_len = addr.size - vec_bytes_;
        uint32_t alloc_len = (payload_len + 4095u) & ~4095u;
        uint8_t* pbuf = static_cast<uint8_t*>(std::aligned_alloc(4096, alloc_len));
        std::memcpy(pbuf, buf + vec_bytes_, payload_len);
        payload_cache_[addr.offset] = AlignedBufPtr(pbuf);
    }
    ctx_.stats().total_reranked++;
}

void RerankConsumer::ConsumePayload(uint8_t* buf, AddressEntry addr) {
    payload_cache_[addr.offset] = AlignedBufPtr(buf);
    ctx_.stats().total_payload_prefetched++;
}

bool RerankConsumer::HasPayload(uint64_t offset) const {
    return payload_cache_.count(offset) > 0;
}

AlignedBufPtr RerankConsumer::TakePayload(uint64_t offset) {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return nullptr;
    auto buf = std::move(it->second);
    payload_cache_.erase(it);
    return buf;
}

void RerankConsumer::CachePayload(uint64_t offset, AlignedBufPtr buf) {
    payload_cache_[offset] = std::move(buf);
}

void RerankConsumer::CleanupUnusedCache(
    const std::vector<CollectorEntry>& final_results) {
    std::unordered_set<uint64_t> needed;
    for (const auto& entry : final_results) {
        needed.insert(entry.addr.offset);
    }
    for (auto it = payload_cache_.begin(); it != payload_cache_.end();) {
        if (needed.count(it->first) == 0) {
            it = payload_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace query
}  // namespace vdb
