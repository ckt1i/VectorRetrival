#include "vdb/query/rerank_consumer.h"

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
        // Copy payload portion to cache
        uint32_t payload_len = addr.size - vec_bytes_;
        auto payload_buf = std::make_unique<uint8_t[]>(payload_len);
        std::memcpy(payload_buf.get(), buf + vec_bytes_, payload_len);
        payload_cache_[addr.offset] = std::move(payload_buf);
    }
    ctx_.stats().total_reranked++;
}

void RerankConsumer::ConsumePayload(uint8_t* buf, AddressEntry addr) {
    payload_cache_[addr.offset] = std::unique_ptr<uint8_t[]>(buf);
    ctx_.stats().total_payload_prefetched++;
}

bool RerankConsumer::HasPayload(uint64_t offset) const {
    return payload_cache_.count(offset) > 0;
}

std::unique_ptr<uint8_t[]> RerankConsumer::TakePayload(uint64_t offset) {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return nullptr;
    auto buf = std::move(it->second);
    payload_cache_.erase(it);
    return buf;
}

void RerankConsumer::CachePayload(uint64_t offset,
                                   std::unique_ptr<uint8_t[]> buf) {
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
