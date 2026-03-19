#pragma once

#include "vdb/common/types.h"

namespace vdb {

/// Compute squared L2 distance between two vectors.
/// Thin wrapper delegating to simd::L2Sqr (AVX2 + scalar fallback).
float L2Sqr(const float* a, const float* b, Dim dim);

}  // namespace vdb
