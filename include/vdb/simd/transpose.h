#pragma once

#include <cstdint>
#include "vdb/common/macros.h"

namespace vdb {
namespace simd {

/// Transpose K streams of N uint32_t values (SoA) into N×8 interleaved layout.
///
/// Input:  K separate arrays, each of length count_per_stream.
/// Output: interleaved buffer of size count_per_stream × 8.
///         interleaved_out[j * 8 + k] = streams_in[k][j]  for k < num_streams
///         interleaved_out[j * 8 + k] = 0                  for k >= num_streams
///
/// num_streams must be <= 8. If < 8, the excess lanes are zero-filled.
///
/// @param streams_in        Array of num_streams pointers, each pointing to
///                          count_per_stream uint32_t values
/// @param interleaved_out   Output buffer (at least count_per_stream × 8 elements)
/// @param num_streams       Number of active streams (1..8)
/// @param count_per_stream  Number of elements per stream
void Transpose8xN(const uint32_t* const* streams_in,
                   uint32_t*             interleaved_out,
                   uint32_t              num_streams,
                   uint32_t              count_per_stream);

/// Inverse transpose: N×8 interleaved layout back to K separate arrays (SoA).
///
/// Input:  interleaved buffer of size count_per_stream × 8.
/// Output: K separate arrays, each of length count_per_stream.
///         streams_out[k][j] = interleaved_in[j * 8 + k]
///
/// @param interleaved_in    Input buffer (count_per_stream × 8 elements)
/// @param streams_out       Array of num_streams pointers, each pointing to
///                          count_per_stream uint32_t values
/// @param num_streams       Number of active streams to extract (1..8)
/// @param count_per_stream  Number of elements per stream
void TransposeNx8(const uint32_t*  interleaved_in,
                   uint32_t* const* streams_out,
                   uint32_t         num_streams,
                   uint32_t         count_per_stream);

}  // namespace simd
}  // namespace vdb
