#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vdb/common/status.h"

namespace vdb {
namespace index {

struct FaissCoarseBuildConfig {
    uint32_t nlist = 0;
    uint32_t dim = 0;
    uint32_t train_size = 0;
    uint32_t niter = 0;
    uint32_t nredo = 1;
    uint64_t seed = 42;
    std::string metric = "l2";
};

struct FaissCoarseBuildResult {
    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    std::string effective_metric = "l2";
    std::string requested_metric = "l2";
};

Status RunFaissCoarseBuilder(const float* vectors,
                             uint32_t num_vectors,
                             const FaissCoarseBuildConfig& config,
                             FaissCoarseBuildResult* result);

}  // namespace index
}  // namespace vdb
