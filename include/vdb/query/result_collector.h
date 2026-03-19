#pragma once

#include <algorithm>
#include <queue>
#include <vector>

#include "vdb/common/types.h"

namespace vdb {
namespace query {

/// Top-K entry — independent struct used by RerankConsumer and OverlapScheduler.
struct CollectorEntry {
    float distance;
    AddressEntry addr;

    bool operator<(const CollectorEntry& o) const {
        return distance < o.distance;  // max-heap: smallest distance at bottom
    }
};

/// Max-heap based Top-K collector.
/// Maintains up to top_k entries with the smallest distances.
class ResultCollector {
 public:
    explicit ResultCollector(uint32_t top_k);

    /// Try to insert. Returns true if the entry entered the heap.
    bool TryInsert(float distance, AddressEntry addr);

    /// Current worst (largest) distance in the heap.
    float TopDistance() const;

    /// Whether the heap has reached top_k entries.
    bool Full() const;

    /// Current heap size.
    uint32_t Size() const;

    /// Extract sorted results (distance ascending). Empties the heap.
    std::vector<CollectorEntry> Finalize();

 private:
    uint32_t top_k_;
    std::priority_queue<CollectorEntry> heap_;
};

}  // namespace query
}  // namespace vdb
