#pragma once

#include <cstdint>

namespace vdb {
namespace simd {

/// Compute Σᵢ q[i] × s(i) where s(i) = (bit[i] ? +1.0 : -1.0),
/// with bit[i] = (sign_bits[i/64] >> (i%64)) & 1.
///
/// Used by epsilon-ip calibration to compute the "accurate" inner product
/// of a full-precision rotated query with a 1-bit sign code, replacing the
/// scalar bit-extract + fmadd loop (~400-500 ns/call for dim=128).
///
/// AVX-512 path: processes 16 floats per iteration using mask-based blend.
/// Scalar fallback: matches original loop semantics exactly.
///
/// @param q          Full-precision float array (length = dim)
/// @param sign_bits  Packed sign bits (length = ceil(dim/64) uint64_t words)
/// @param dim        Dimensionality (arbitrary, non-multiple-of-16 handled)
/// @return           Σᵢ q[i] × (2·bit[i] − 1)
float SignedDotFromBits(const float* q, const uint64_t* sign_bits,
                        uint32_t dim);

}  // namespace simd
}  // namespace vdb
