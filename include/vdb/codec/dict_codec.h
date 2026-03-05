#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vdb {
namespace codec {

/// Result of DictCodec::BuildDict.
struct DictCodecResult {
    /// Unique string entries in insertion-order.
    /// dict[i] is the string corresponding to index i.
    std::vector<std::string> dict;

    /// Index of each input value in `dict` (parallel to the input array).
    /// indices[j] == k  iff  input[j] == dict[k].
    std::vector<uint32_t> indices;
};

/// Dictionary codec for string payload columns.
///
/// Each payload column (e.g. a category field) is encoded as:
///   - A compact dict[] of unique strings (stored once in the DataFile header)
///   - A dense indices[] array (one uint32_t per record)
///
/// The indices can be further compressed with BitpackCodec:
///   bit_width = ComputeMinBitWidth(indices.data(), indices.size())
///
/// This is a simple implementation using std::unordered_map.
/// See UNDO.txt [PHASE3-005] for potential optimizations:
///   - Frequency-sorted dict + smaller index types
///   - Integration with BitpackCodec for on-disk index compression
///   - Perfect hashing for decode
class DictCodec {
public:
    DictCodec() = delete;  // Static-only utility class

    /// Build a dictionary from a sequence of string values.
    ///
    /// Each unique value is assigned an index in insertion order.
    /// Duplicate values reuse the existing index.
    ///
    /// @param values  Input strings (may contain duplicates)
    /// @return        DictCodecResult with parallel dict[] and indices[]
    static DictCodecResult BuildDict(const std::vector<std::string>& values);

    /// Decode a sequence of indices back to strings using a dictionary.
    ///
    /// @param indices  Index array (each entry must be < dict.size())
    /// @param dict     Dictionary built by BuildDict
    /// @return         Decoded string values (same length as indices)
    static std::vector<std::string> Decode(const std::vector<uint32_t>&  indices,
                                           const std::vector<std::string>& dict);
};

}  // namespace codec
}  // namespace vdb
