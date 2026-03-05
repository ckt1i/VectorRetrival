#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vdb/simd/popcount.h"

using vdb::simd::Popcount64;
using vdb::simd::PopcountXor;
using vdb::simd::PopcountTotal;

// ===========================================================================
// Popcount64 — single word
// ===========================================================================

TEST(Popcount64Test, Zero) {
    EXPECT_EQ(Popcount64(0ULL), 0u);
}

TEST(Popcount64Test, AllOnes) {
    EXPECT_EQ(Popcount64(~0ULL), 64u);
}

TEST(Popcount64Test, SingleBit) {
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(Popcount64(1ULL << i), 1u) << "bit " << i;
    }
}

TEST(Popcount64Test, KnownValues) {
    EXPECT_EQ(Popcount64(0x5555555555555555ULL), 32u);  // alternating bits
    EXPECT_EQ(Popcount64(0xAAAAAAAAAAAAAAAAULL), 32u);
    EXPECT_EQ(Popcount64(0xFF00FF00FF00FF00ULL), 32u);
    EXPECT_EQ(Popcount64(0x0F0F0F0F0F0F0F0FULL), 32u);
}

TEST(Popcount64Test, FewBits) {
    EXPECT_EQ(Popcount64(0b1010110ULL), 4u);
    EXPECT_EQ(Popcount64(0b11111ULL), 5u);
    EXPECT_EQ(Popcount64(0b10000000ULL), 1u);
}

// ===========================================================================
// PopcountXor — batch XOR popcount
// ===========================================================================

TEST(PopcountXorTest, IdenticalCodes) {
    // XOR of identical = 0 → popcount = 0
    std::vector<uint64_t> a = {0xDEADBEEF01234567ULL, 0x0123456789ABCDEFULL};
    EXPECT_EQ(PopcountXor(a.data(), a.data(), 2), 0u);
}

TEST(PopcountXorTest, AllDifferent) {
    std::vector<uint64_t> a = {0ULL};
    std::vector<uint64_t> b = {~0ULL};
    EXPECT_EQ(PopcountXor(a.data(), b.data(), 1), 64u);
}

TEST(PopcountXorTest, SingleWord) {
    uint64_t a = 0xFF;   // 8 bits set
    uint64_t b = 0x0F;   // 4 bits set
    // XOR = 0xF0 → popcount = 4
    EXPECT_EQ(PopcountXor(&a, &b, 1), 4u);
}

TEST(PopcountXorTest, MultipleWords) {
    // 4 words: known XOR pattern
    std::vector<uint64_t> a(4, 0xAAAAAAAAAAAAAAAAULL);
    std::vector<uint64_t> b(4, 0x5555555555555555ULL);
    // Each XOR = 0xFFFF... → 64 bits per word × 4 = 256
    EXPECT_EQ(PopcountXor(a.data(), b.data(), 4), 256u);
}

TEST(PopcountXorTest, LargeInput) {
    // Simulate dim=512 → 512/64 = 8 words
    const uint32_t num_words = 8;
    std::vector<uint64_t> a(num_words, 0ULL);
    std::vector<uint64_t> b(num_words, 0ULL);

    // Set specific bits: every other bit in a, complementary in b
    for (uint32_t w = 0; w < num_words; ++w) {
        a[w] = (w % 2 == 0) ? 0xFFFFFFFF00000000ULL : 0x00000000FFFFFFFFULL;
        b[w] = ~a[w];
    }
    // Each word: XOR = all ones → 64 bits × 8 = 512
    EXPECT_EQ(PopcountXor(a.data(), b.data(), num_words), 512u);
}

TEST(PopcountXorTest, ZeroWords) {
    uint64_t a = 1;
    uint64_t b = 1;
    EXPECT_EQ(PopcountXor(&a, &b, 0), 0u);
}

TEST(PopcountXorTest, NonMultipleOf4Words) {
    // 5 words: tests scalar tail after AVX2 main loop
    std::vector<uint64_t> a(5, 0xF0F0F0F0F0F0F0F0ULL);
    std::vector<uint64_t> b(5, 0x0F0F0F0F0F0F0F0FULL);
    // Each XOR = 0xFFFF... → 64 × 5 = 320
    EXPECT_EQ(PopcountXor(a.data(), b.data(), 5), 320u);
}

TEST(PopcountXorTest, ThreeWords) {
    // 3 words: entirely in the scalar tail for AVX2 (needs >=4 for main loop)
    std::vector<uint64_t> a(3, 0x0101010101010101ULL);  // 8 bits each
    std::vector<uint64_t> b(3, 0ULL);
    // XOR = a → popcount = 8 × 3 = 24
    EXPECT_EQ(PopcountXor(a.data(), b.data(), 3), 24u);
}

// ===========================================================================
// PopcountTotal — total set bits
// ===========================================================================

TEST(PopcountTotalTest, AllZero) {
    std::vector<uint64_t> code(4, 0ULL);
    EXPECT_EQ(PopcountTotal(code.data(), 4), 0u);
}

TEST(PopcountTotalTest, AllOnes) {
    std::vector<uint64_t> code(2, ~0ULL);
    EXPECT_EQ(PopcountTotal(code.data(), 2), 128u);
}

TEST(PopcountTotalTest, KnownPattern) {
    // 1 word with exactly 10 bits set
    uint64_t code = 0b1111111111ULL;  // 10 bits
    EXPECT_EQ(PopcountTotal(&code, 1), 10u);
}

TEST(PopcountTotalTest, LargeArray) {
    // 16 words, each with 32 bits set
    std::vector<uint64_t> code(16, 0x5555555555555555ULL);
    EXPECT_EQ(PopcountTotal(code.data(), 16), 16u * 32u);
}

TEST(PopcountTotalTest, MixedValues) {
    std::vector<uint64_t> code = {
        0x01ULL,  // 1 bit
        0x03ULL,  // 2 bits
        0x07ULL,  // 3 bits
        0x0FULL,  // 4 bits
        0x1FULL,  // 5 bits
    };
    EXPECT_EQ(PopcountTotal(code.data(), 5), 1u + 2u + 3u + 4u + 5u);
}

TEST(PopcountTotalTest, ZeroWords) {
    uint64_t dummy = ~0ULL;
    EXPECT_EQ(PopcountTotal(&dummy, 0), 0u);
}

// ===========================================================================
// Consistency: PopcountXor(a, zero) == PopcountTotal(a)
// ===========================================================================

TEST(PopcountConsistencyTest, XorWithZeroEqualsTotal) {
    std::vector<uint64_t> a = {0xDEADBEEFULL, 0x12345678ULL, 0xCAFEBABEULL};
    std::vector<uint64_t> z(3, 0ULL);

    EXPECT_EQ(PopcountXor(a.data(), z.data(), 3),
              PopcountTotal(a.data(), 3));
}
