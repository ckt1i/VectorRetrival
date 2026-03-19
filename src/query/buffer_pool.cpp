#include "vdb/query/buffer_pool.h"

namespace vdb {
namespace query {

uint8_t* BufferPool::Acquire(uint32_t size) {
    // Find a pooled buffer with sufficient capacity
    for (size_t i = 0; i < pool_.size(); ++i) {
        if (pool_[i].capacity >= size) {
            uint8_t* raw = pool_[i].buf.release();
            uint32_t cap = pool_[i].capacity;
            // Swap-and-pop removal
            pool_[i] = std::move(pool_.back());
            pool_.pop_back();
            outstanding_[raw] = cap;
            return raw;
        }
    }
    // No suitable buffer in pool — allocate new
    auto buf = std::make_unique<uint8_t[]>(size);
    uint8_t* raw = buf.get();
    outstanding_[raw] = size;
    buf.release();
    return raw;
}

void BufferPool::Release(uint8_t* buf) {
    auto it = outstanding_.find(buf);
    if (it == outstanding_.end()) return;
    uint32_t cap = it->second;
    outstanding_.erase(it);
    pool_.push_back({std::unique_ptr<uint8_t[]>(buf), cap});
}

}  // namespace query
}  // namespace vdb
