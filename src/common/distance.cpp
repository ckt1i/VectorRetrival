#include "vdb/common/distance.h"
#include "vdb/simd/distance_l2.h"

namespace vdb {

float L2Sqr(const float* a, const float* b, Dim dim) {
    return simd::L2Sqr(a, b, dim);
}

}  // namespace vdb
