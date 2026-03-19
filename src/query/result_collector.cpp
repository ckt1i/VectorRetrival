#include "vdb/query/result_collector.h"

#include <algorithm>
#include <limits>

namespace vdb {
namespace query {

ResultCollector::ResultCollector(uint32_t top_k) : top_k_(top_k) {}

bool ResultCollector::TryInsert(float distance, AddressEntry addr) {
    if (heap_.size() < top_k_) {
        heap_.push({distance, addr});
        return true;
    }
    if (distance < heap_.top().distance) {
        heap_.pop();
        heap_.push({distance, addr});
        return true;
    }
    return false;
}

float ResultCollector::TopDistance() const {
    if (heap_.empty()) return std::numeric_limits<float>::max();
    return heap_.top().distance;
}

bool ResultCollector::Full() const {
    return heap_.size() >= top_k_;
}

uint32_t ResultCollector::Size() const {
    return static_cast<uint32_t>(heap_.size());
}

std::vector<CollectorEntry> ResultCollector::Finalize() {
    std::vector<CollectorEntry> results;
    results.reserve(heap_.size());
    while (!heap_.empty()) {
        results.push_back(heap_.top());
        heap_.pop();
    }
    // Heap gave us largest-first; reverse to get ascending distance
    std::reverse(results.begin(), results.end());
    return results;
}

}  // namespace query
}  // namespace vdb
