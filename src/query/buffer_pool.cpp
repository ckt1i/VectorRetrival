#include "vdb/query/buffer_pool.h"

#include <cstdio>
#include <cstdlib>

namespace vdb {
namespace query {

uint8_t* BufferPool::Acquire(uint32_t size) {
    uint32_t aligned_size = (size + 4095u) & ~4095u;

    // Find a pooled buffer with sufficient capacity
    for (size_t i = 0; i < pool_.size(); ++i) {
        if (pool_[i].capacity >= aligned_size) {
            uint8_t* raw = pool_[i].buf;
            uint32_t cap = pool_[i].capacity;
            // Swap-and-pop removal
            pool_[i] = std::move(pool_.back());
            pool_.pop_back();
            outstanding_[raw] = cap;
            return raw;
        }
    }
    // No suitable buffer in pool — allocate 4KB-aligned
    uint8_t* raw = static_cast<uint8_t*>(
        std::aligned_alloc(4096, aligned_size));
    if (!raw) {
        std::fprintf(stderr, "FATAL: aligned_alloc failed in BufferPool (%u bytes)\n", aligned_size);
        std::abort();
    }
    outstanding_[raw] = aligned_size;
    return raw;
}

void BufferPool::Release(uint8_t* buf) {
    auto it = outstanding_.find(buf);
    if (it == outstanding_.end()) return;
    uint32_t cap = it->second;
    outstanding_.erase(it);
    pool_.push_back({buf, cap});
}

void BufferPool::Prime(uint32_t size, uint32_t count) {
    uint32_t aligned_size = (size + 4095u) & ~4095u;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* raw = static_cast<uint8_t*>(
            std::aligned_alloc(4096, aligned_size));
        if (!raw) {
            std::fprintf(stderr,
                         "FATAL: aligned_alloc failed in BufferPool::Prime (%u bytes)\n",
                         aligned_size);
            std::abort();
        }
        pool_.push_back({raw, aligned_size});
    }
}

}  // namespace query
}  // namespace vdb
