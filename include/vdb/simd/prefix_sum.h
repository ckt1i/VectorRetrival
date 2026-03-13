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

/// Compute exclusive prefix sums for K independent streams simultaneously.
///
/// Input and output are in **interleaved layout**: for K streams each of
/// length `count`, the buffer has `count × 8` uint32_t elements where
///   buf[j * 8 + k] = stream_k[j]
///
/// The function treats each SIMD lane as an independent prefix sum:
///   out[j * 8 + k] = sum of in[0*8+k] + in[1*8+k] + ... + in[(j-1)*8+k]
///   out[0 * 8 + k] = 0  (exclusive)
///
/// When num_streams < 8, excess lanes in the input must be zero; the
/// corresponding output lanes will also be zero.
///
/// Dispatch (compile-time):
///   - VDB_USE_AVX2: one _mm256_add_epi32 per element index (G iterations
///     for G elements per stream). No lane-crossing dependency.
///   - Otherwise: scalar loop over K streams × count elements.
///
/// @param interleaved_in   Interleaved input  (count × 8 elements)
/// @param interleaved_out  Interleaved output (count × 8 elements; may not alias in)
/// @param count            Number of elements per stream
/// @param num_streams      Number of active streams (1..8)
void ExclusivePrefixSumMulti(const uint32_t* VDB_RESTRICT interleaved_in,
                              uint32_t* VDB_RESTRICT       interleaved_out,
                              uint32_t                     count,
                              uint32_t                     num_streams);

}  // namespace simd
}  // namespace vdb
