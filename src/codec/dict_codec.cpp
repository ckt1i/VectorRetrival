#include "vdb/codec/dict_codec.h"

#include <unordered_map>

namespace vdb {
namespace codec {

DictCodecResult DictCodec::BuildDict(const std::vector<std::string>& values) {
    // Map from string value → index in the dict vector.
    // Uses insertion-order assignment so dict indices are stable across calls
    // with the same value sequence.
    // See UNDO.txt [PHASE3-005] for frequency-sorted and perfect-hash variants.
    std::unordered_map<std::string, uint32_t> index_map;
    index_map.reserve(values.size());

    DictCodecResult result;
    result.indices.reserve(values.size());

    for (const std::string& val : values) {
        auto it = index_map.find(val);
        if (it == index_map.end()) {
            const uint32_t new_idx = static_cast<uint32_t>(result.dict.size());
            index_map.emplace(val, new_idx);
            result.dict.push_back(val);
            result.indices.push_back(new_idx);
        } else {
            result.indices.push_back(it->second);
        }
    }

    return result;
}

std::vector<std::string> DictCodec::Decode(const std::vector<uint32_t>&   indices,
                                            const std::vector<std::string>& dict) {
    std::vector<std::string> result;
    result.reserve(indices.size());
    for (const uint32_t idx : indices) {
        // Bounds check: silently emit an empty string for out-of-range indices
        // rather than throwing, so callers that tolerate partial data can continue.
        result.push_back(idx < dict.size() ? dict[idx] : std::string{});
    }
    return result;
}

}  // namespace codec
}  // namespace vdb
