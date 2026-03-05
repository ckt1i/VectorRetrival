#pragma once

#include <cstdint>
#include "vdb/common/macros.h"

namespace vdb {
namespace simd {

/// Count the number of set bits in a single 64-bit word.
///
/// Uses `__builtin_popcountll` on GCC/Clang (hardware POPCNT when available).
/// Falls back to software Hamming-weight on other compilers.
///
/// @param x  64-bit input word
/// @return   Number of bits set to 1 (0..64)
VDB_FORCE_INLINE uint32_t Popcount64(uint64_t x) {
#ifdef VDB_COMPILER_GCC_LIKE
    return static_cast<uint32_t>(__builtin_popcountll(x));
#else
    // Fallback: Hamming weight via bit manipulation
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<uint32_t>((x * 0x0101010101010101ULL) >> 56);
#endif
}

/// Compute popcount(a[i] XOR b[i]) summed over `num_words` uint64_t words.
///
/// This is the core primitive for RaBitQ 1-bit distance estimation.
/// Given two binary codes packed as uint64_t arrays, the result equals the
/// Hamming distance between them, which is used in the formula:
///
///   dot(q', x) = num_bits_set_in_x - 2 * hamming_dist(q_sign, x)
///
/// Equivalently, if we XOR the query sign bits with the database code,
/// popcount gives the number of differing dimensions.
///
/// Implementation dispatches at compile time:
///   - VDB_USE_AVX2: vectorized accumulation via lookup-table popcount
///     (Harley-Seal or VPSHUFB-based). Processes 4 uint64_t per iteration.
///   - Otherwise: scalar loop using Popcount64.
///
/// @param a          First binary code  (at least `num_words` uint64_t)
/// @param b          Second binary code (at least `num_words` uint64_t)
/// @param num_words  Number of uint64_t words (dim / 64 for 1-bit codes)
/// @return           Total popcount of (a[0]^b[0]) + ... + (a[n-1]^b[n-1])
uint32_t PopcountXor(const uint64_t* VDB_RESTRICT a,
                     const uint64_t* VDB_RESTRICT b,
                     uint32_t num_words);

/// Compute popcount of a single binary code (sum of set bits).
///
/// Used to compute Σx[i] for the RaBitQ inner product formula:
///   ⟨q̄, ô⟩ = (2/√L)(q'·x) - (1/√L)Σq'ᵢ
///
/// The term Σx[i] = total popcount of the code.
///
/// @param code       Binary code packed as uint64_t array
/// @param num_words  Number of uint64_t words
/// @return           Total number of set bits
uint32_t PopcountTotal(const uint64_t* code, uint32_t num_words);

}  // namespace simd
}  // namespace vdb
