#pragma once

#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace vdb {
namespace query {

/// Simple buffer pool for reusing I/O buffers.
/// Single-threaded only — no locking.
class BufferPool {
 public:
    BufferPool() = default;
    ~BufferPool() {
        for (auto& e : pool_) std::free(e.buf);
        // outstanding_ buffers are owned by callers — not freed here
    }

    /// Acquire a buffer of at least `size` bytes.
    /// Reuses a pooled buffer if one with sufficient capacity exists,
    /// otherwise allocates a new one.
    /// @return Raw pointer (caller must Release when done)
    uint8_t* Acquire(uint32_t size);

    /// Return a buffer to the pool for reuse.
    void Release(uint8_t* buf);

    /// Number of buffers currently in the pool (available for reuse).
    uint32_t PoolSize() const {
        return static_cast<uint32_t>(pool_.size());
    }

    /// Number of outstanding (acquired but not released) buffers.
    uint32_t OutstandingCount() const {
        return static_cast<uint32_t>(outstanding_.size());
    }

 private:
    struct PoolEntry {
        uint8_t* buf;
        uint32_t capacity;
        ~PoolEntry() = default;
    };

    std::vector<PoolEntry> pool_;
    // Track outstanding buffers: raw ptr → capacity
    std::unordered_map<uint8_t*, uint32_t> outstanding_;
};

}  // namespace query
}  // namespace vdb
