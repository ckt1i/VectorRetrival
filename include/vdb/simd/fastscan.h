#pragma once

#include <cstdint>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace simd {

/// VPSHUFB batch-32 accumulation of packed 4-bit LUT lookups.
///
/// Processes 32 vectors simultaneously using VPSHUFB. The packed_codes must
/// be in block-32 nibble-interleaved layout (as produced by PackSignBitsForFastScan).
/// The LUT must be in matching AVX-512/AVX2 lane layout (from BuildFastScanLUT).
///
/// Uses two-plane accumulation (lo/hi bytes) to avoid int16 overflow.
/// After combining: result[v] = lo_plane[v] + (hi_plane[v] << 8)
///
/// @param packed_codes  Packed sign bits, block-32 layout (dim*4 bytes)
/// @param lut           Packed LUT, lo+hi byte planes (dim*8 bytes, 64-byte aligned)
/// @param result        Output: 32 raw accumulated values (caller owns, >= 32 entries)
/// @param dim           Vector dimensionality (must be multiple of 4)
void AccumulateBlock(const uint8_t* VDB_RESTRICT packed_codes,
                     const uint8_t* VDB_RESTRICT lut,
                     uint32_t* VDB_RESTRICT result,
                     Dim dim);

/// Build the packed VPSHUFB LUT from a 14-bit quantized query.
///
/// For each group of 4 dimensions, builds a 16-entry lookup table mapping
/// every 4-bit nibble pattern to the sum of corresponding quantized query
/// values. The LUT entries are shifted to unsigned, split into lo/hi byte
/// planes, and arranged for AVX-512 128-bit lane structure.
///
/// @param quant_query   14-bit signed quantized query (int16_t, length = dim)
/// @param lut_out       Output LUT buffer (dim*8 bytes, must be 64-byte aligned)
/// @param dim           Vector dimensionality (must be multiple of 4)
/// @return              Accumulated shift (sum of v_min per group, for de-quantization)
int32_t BuildFastScanLUT(const int16_t* VDB_RESTRICT quant_query,
                          uint8_t* VDB_RESTRICT lut_out,
                          Dim dim);

/// Quantize a unit query vector to 14-bit signed integers.
///
/// Symmetric quantization: width = max(|q|) / 8191.
/// Each q[i] is rounded to the nearest int16 in [-8191, 8191].
/// The sum of any 4 consecutive values fits in int16 without overflow.
///
/// @param query         Unit query vector (float, length = dim)
/// @param quant_out     Output: quantized values (int16_t, length = dim)
/// @param dim           Vector dimensionality
/// @return              Quantization step width (for de-quantization: value ≈ quant * width)
float QuantizeQuery14Bit(const float* VDB_RESTRICT query,
                          int16_t* VDB_RESTRICT quant_out,
                          Dim dim);

}  // namespace simd
}  // namespace vdb
