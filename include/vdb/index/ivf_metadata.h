#pragma once

#include <cstdint>

namespace vdb {
namespace index {

enum class AssignmentMode : uint8_t {
    Single = 0,
    RedundantTop2Naive = 1,
    RedundantTop2Rair = 2,
};

enum class ClusteringSource : uint8_t {
    Auto = 0,
    Precomputed = 1,
};

}  // namespace index
}  // namespace vdb
