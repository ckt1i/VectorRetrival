#include <gtest/gtest.h>
#include <vector>

#include "vdb/simd/bit_unpack.h"

using vdb::simd::BitUnpack;

// Helper: unpack into a vector for convenient comparison
static std::vector<uint32_t> Unpack(const std::vector<uint8_t>& packed,
                                    uint8_t bit_width, uint32_t count) {
    std::vector<uint32_t> out(count);
    BitUnpack(packed.data(), bit_width, out.data(), count);
    return out;
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(BitUnpackTest, ZeroCount) {
    std::vector<uint8_t> packed = {0xFF};
    std::vector<uint32_t> out(1, 42u);
    BitUnpack(packed.data(), 1, out.data(), 0);
    // Output must remain untouched
    EXPECT_EQ(out[0], 42u);
}

// ---------------------------------------------------------------------------
// 1-bit: the AVX2 specialized path (most critical for RaBitQ)
// ---------------------------------------------------------------------------

TEST(BitUnpackTest, OneBit_AllZeros_8Elements) {
    // 0b00000000 → [0,0,0,0,0,0,0,0]
    auto result = Unpack({0x00}, 1, 8);
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(BitUnpackTest, OneBit_AllOnes_8Elements) {
    // 0b11111111 → [1,1,1,1,1,1,1,1]
    auto result = Unpack({0xFF}, 1, 8);
    EXPECT_EQ(result, (std::vector<uint32_t>{1, 1, 1, 1, 1, 1, 1, 1}));
}

TEST(BitUnpackTest, OneBit_LSBFirst_8Elements) {
    // 0b10101010 = 0xAA, LSB-first → element0=bit0=0, element1=bit1=1, ...
    auto result = Unpack({0xAA}, 1, 8);
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 0, 1, 0, 1, 0, 1}));
}

TEST(BitUnpackTest, OneBit_SingleBitSet_8Elements) {
    // 0b00000001 = 0x01 → only element 0 is 1
    auto result = Unpack({0x01}, 1, 8);
    EXPECT_EQ(result, (std::vector<uint32_t>{1, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(BitUnpackTest, OneBit_HighBitSet_8Elements) {
    // 0b10000000 = 0x80 → only element 7 is 1
    auto result = Unpack({0x80}, 1, 8);
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 0, 0, 0, 0, 0, 0, 1}));
}

TEST(BitUnpackTest, OneBit_64Elements_AllOnes) {
    // Primary use case: one full AddressBlock (64 records)
    std::vector<uint8_t> packed(8, 0xFF);  // 8 bytes × 8 bits = 64 ones
    std::vector<uint32_t> expected(64, 1u);
    auto result = Unpack(packed, 1, 64);
    EXPECT_EQ(result, expected);
}

TEST(BitUnpackTest, OneBit_64Elements_AllZeros) {
    std::vector<uint8_t> packed(8, 0x00);
    std::vector<uint32_t> expected(64, 0u);
    auto result = Unpack(packed, 1, 64);
    EXPECT_EQ(result, expected);
}

TEST(BitUnpackTest, OneBit_64Elements_Alternating) {
    // 0xAA = 0b10101010 repeated × 8 → [0,1,0,1,...] × 8 bytes = 64 elements
    std::vector<uint8_t> packed(8, 0xAA);
    std::vector<uint32_t> expected;
    for (int i = 0; i < 64; i++) expected.push_back(i % 2);  // 0,1,0,1,...
    auto result = Unpack(packed, 1, 64);
    EXPECT_EQ(result, expected);
}

TEST(BitUnpackTest, OneBit_NonMultipleOf8_ScalarTail) {
    // 11 elements: 1 full byte (8 bits) + 3 tail bits
    // packed[0] = 0xFF → elements 0..7 = 1
    // packed[1] = 0x05 = 0b00000101 → elements 8,9,10 = 1,0,1
    std::vector<uint8_t> packed = {0xFF, 0x05};
    auto result = Unpack(packed, 1, 11);
    std::vector<uint32_t> expected = {1,1,1,1,1,1,1,1, 1,0,1};
    EXPECT_EQ(result, expected);
}

// ---------------------------------------------------------------------------
// 2-bit: scalar generic path
// ---------------------------------------------------------------------------

TEST(BitUnpackTest, TwoBit_KnownValues_4Elements) {
    // 0xE4 = 0b11100100, LSB-first:
    //   bits[0:1] = 00 → 0
    //   bits[2:3] = 01 → 1
    //   bits[4:5] = 10 → 2
    //   bits[6:7] = 11 → 3
    auto result = Unpack({0xE4}, 2, 4);
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 2, 3}));
}

TEST(BitUnpackTest, TwoBit_AllMax_4Elements) {
    // 0xFF: all 2-bit values = 3
    auto result = Unpack({0xFF}, 2, 4);
    EXPECT_EQ(result, (std::vector<uint32_t>{3, 3, 3, 3}));
}

TEST(BitUnpackTest, TwoBit_AllZero_4Elements) {
    auto result = Unpack({0x00}, 2, 4);
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 0, 0, 0}));
}

// ---------------------------------------------------------------------------
// 4-bit: scalar generic path
// ---------------------------------------------------------------------------

TEST(BitUnpackTest, FourBit_KnownValues_4Elements) {
    // packed = [0x10, 0x32]
    // byte0 = 0x10 = 0b00010000: low nibble=0, high nibble=1
    // byte1 = 0x32 = 0b00110010: low nibble=2, high nibble=3
    auto result = Unpack({0x10, 0x32}, 4, 4);
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 2, 3}));
}

TEST(BitUnpackTest, FourBit_AllOnes_2Elements) {
    // 0xFF: low nibble=0xF=15, high nibble=0xF=15
    auto result = Unpack({0xFF}, 4, 2);
    EXPECT_EQ(result, (std::vector<uint32_t>{15, 15}));
}

// ---------------------------------------------------------------------------
// 8-bit: scalar generic path (trivial byte-to-uint32 widening)
// ---------------------------------------------------------------------------

TEST(BitUnpackTest, EightBit_KnownValues_4Elements) {
    auto result = Unpack({1, 2, 3, 4}, 8, 4);
    EXPECT_EQ(result, (std::vector<uint32_t>{1, 2, 3, 4}));
}

TEST(BitUnpackTest, EightBit_MaxValue) {
    auto result = Unpack({255}, 8, 1);
    EXPECT_EQ(result, (std::vector<uint32_t>{255}));
}

TEST(BitUnpackTest, EightBit_Zero) {
    auto result = Unpack({0}, 8, 1);
    EXPECT_EQ(result, (std::vector<uint32_t>{0}));
}

// ---------------------------------------------------------------------------
// Round-trip: encode with BitpackCodec then decode with BitUnpack
// (uses the codec from Phase 3 codec tests but duplicated here for isolation)
// ---------------------------------------------------------------------------

TEST(BitUnpackTest, OneBit_RoundTrip_ManualEncode) {
    // Manually encode [1,0,1,0,1,0,1,0] as 1-bit: 0b01010101 = 0x55
    std::vector<uint32_t> values = {1, 0, 1, 0, 1, 0, 1, 0};
    uint8_t packed_byte = 0;
    for (int i = 0; i < 8; i++) packed_byte |= (values[i] << i);
    EXPECT_EQ(packed_byte, 0x55u);

    auto result = Unpack({packed_byte}, 1, 8);
    EXPECT_EQ(result, values);
}

// ---------------------------------------------------------------------------
// BitUnpack1 — specialized 1-bit API
// ---------------------------------------------------------------------------

using vdb::simd::BitUnpack1;

TEST(BitUnpack1Test, AllOnes_64) {
    // Default overload: count=64 (one AddressBlock)
    std::vector<uint8_t> packed(8, 0xFF);
    std::vector<uint32_t> out(64, 0u);
    BitUnpack1(packed.data(), out.data());
    for (uint32_t v : out) EXPECT_EQ(v, 1u);
}

TEST(BitUnpack1Test, AllZeros_64) {
    std::vector<uint8_t> packed(8, 0x00);
    std::vector<uint32_t> out(64, 42u);  // sentinel
    BitUnpack1(packed.data(), out.data());
    for (uint32_t v : out) EXPECT_EQ(v, 0u);
}

TEST(BitUnpack1Test, Alternating_64) {
    std::vector<uint8_t> packed(8, 0xAA);  // 0b10101010
    std::vector<uint32_t> out(64);
    BitUnpack1(packed.data(), out.data());
    for (uint32_t i = 0; i < 64; i++) {
        EXPECT_EQ(out[i], i % 2) << "at i=" << i;
    }
}

TEST(BitUnpack1Test, ExplicitCount_8) {
    // 3-arg overload with explicit count
    std::vector<uint8_t> packed = {0x55};  // 0b01010101
    std::vector<uint32_t> out(8);
    BitUnpack1(packed.data(), out.data(), 8);
    EXPECT_EQ(out, (std::vector<uint32_t>{1, 0, 1, 0, 1, 0, 1, 0}));
}

TEST(BitUnpack1Test, ExplicitCount_NonAligned) {
    // 5 elements: not a multiple of 8
    std::vector<uint8_t> packed = {0x15};  // 0b00010101 → [1,0,1,0,1]
    std::vector<uint32_t> out(5);
    BitUnpack1(packed.data(), out.data(), 5);
    EXPECT_EQ(out, (std::vector<uint32_t>{1, 0, 1, 0, 1}));
}

TEST(BitUnpack1Test, MatchesGenericBitUnpack) {
    // Verify BitUnpack1 produces identical results to BitUnpack(packed, 1, ...)
    std::vector<uint8_t> packed = {0x3C, 0xA5, 0x0F, 0xF0, 0x55, 0xAA, 0x12, 0x34};
    std::vector<uint32_t> out_generic(64), out_specialized(64);

    BitUnpack(packed.data(), 1, out_generic.data(), 64);
    BitUnpack1(packed.data(), out_specialized.data());

    EXPECT_EQ(out_generic, out_specialized);
}
