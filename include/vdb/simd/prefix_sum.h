#pragma once

#include <cstdint>
#include "vdb/common/macros.h"

namespace vdb {
namespace simd {

/// Compute the **exclusive** prefix sum of `count` uint32_t values.
///
/// Definition:
///   out[0] = 0
///   out[i] = in[0] + in[1] + ... + in[i-1]   for i > 0
///
/// Primary use case: count=64 (one AddressBlock of 64 record sizes).
/// The result feeds AddressEntry decode:
///   offset[i] = base_offset + out[i],  size[i] = in[i]
///
/// Dispatch (compile-time):
///   - VDB_USE_AVX2: uses _mm256_slli_si256 for intra-lane prefix scan
///     (8 uint32_t/register) and _mm_alignr_epi8 for the cross-lane carry.
///     Scalar tail handles count % 8 != 0 remainder.
///   - Otherwise: scalar loop.
///
/// See UNDO.txt [PHASE3-003] for potential loop-unrolling and AVX-512 paths.
///
/// @param in    Input values (length >= count)
/// @param out   Output exclusive prefix sums (length >= count; may not alias in)
/// @param count Number of elements (may be 0)
void ExclusivePrefixSum(const uint32_t* VDB_RESTRICT in,
                        uint32_t* VDB_RESTRICT       out,
                        uint32_t                     count);

}  // namespace simd
}  // namespace vdb
