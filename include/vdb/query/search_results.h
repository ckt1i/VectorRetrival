#pragma once

#include <vector>

#include "vdb/common/types.h"
#include "vdb/query/search_context.h"

namespace vdb {
namespace query {

/// A single search result: distance + parsed payload columns.
struct SearchResult {
    float distance;
    std::vector<Datum> payload;  // One Datum per payload column
};

/// RAII wrapper for search results. Move-only.
class SearchResults {
 public:
    SearchResults() = default;
    ~SearchResults() = default;

    SearchResults(SearchResults&&) = default;
    SearchResults& operator=(SearchResults&&) = default;

    SearchResults(const SearchResults&) = delete;
    SearchResults& operator=(const SearchResults&) = delete;

    /// Number of results.
    uint32_t size() const {
        return static_cast<uint32_t>(results_.size());
    }

    bool empty() const { return results_.empty(); }

    const SearchResult& operator[](uint32_t i) const { return results_[i]; }
    SearchResult& operator[](uint32_t i) { return results_[i]; }

    const std::vector<SearchResult>& results() const { return results_; }
    std::vector<SearchResult>& results() { return results_; }

    const SearchStats& stats() const { return stats_; }
    SearchStats& stats() { return stats_; }

 private:
    std::vector<SearchResult> results_;
    SearchStats stats_;
};

}  // namespace query
}  // namespace vdb
