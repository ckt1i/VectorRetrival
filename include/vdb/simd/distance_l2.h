#pragma once

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace simd {

/// Compute the squared L2 distance between two float vectors of dimension `d`.
///
/// $$\text{L2Sqr}(a, b) = \sum_{i=0}^{d-1}(a_i - b_i)^2$$
///
/// Implementation dispatches at compile time:
///   - VDB_USE_AVX2: processes 8 floats/iteration with _mm256_fmadd_ps.
///     A scalar loop handles the tail (d % 8 != 0).
///   - Otherwise: pure scalar loop.
///
/// Neither pointer needs to be aligned; unaligned loads are used throughout.
/// See UNDO.txt [PHASE3-001] for the deferred AVX-512 path.
/// See UNDO.txt [PHASE3-006] for the aligned-load optimization note.
///
/// @param a  First vector (length >= d)
/// @param b  Second vector (length >= d)
/// @param d  Number of dimensions (may be 0, returns 0.0f)
/// @return   Sum of squared component differences
float L2Sqr(const float* VDB_RESTRICT a, const float* VDB_RESTRICT b, Dim d);

}  // namespace simd
}  // namespace vdb
