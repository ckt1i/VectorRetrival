#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

#include "vdb/codec/bitpack_codec.h"

using vdb::codec::BitpackCodec;

// ---------------------------------------------------------------------------
// ComputeMinBitWidth
// ---------------------------------------------------------------------------

TEST(BitpackCodecTest, MinBitWidth_ZeroCount_Returns1) {
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(nullptr, 0), 1u);
}

TEST(BitpackCodecTest, MinBitWidth_AllZeros) {
    std::vector<uint32_t> v = {0, 0, 0, 0};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 1u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue0) {
    uint32_t val = 0u;
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(&val, 1u), 1u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue1) {
    std::vector<uint32_t> v = {0, 1, 0, 1};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 1u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue2) {
    std::vector<uint32_t> v = {0, 1, 2};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 2u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue3) {
    std::vector<uint32_t> v = {3};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 2u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue4) {
    std::vector<uint32_t> v = {4};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 3u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue255) {
    std::vector<uint32_t> v = {255};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 8u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue256_DefaultThreshold) {
    // 256 == kDefaultMaxPackableValue → still packable (not strictly greater)
    std::vector<uint32_t> v = {256};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 9u);
}

TEST(BitpackCodecTest, MinBitWidth_MaxValue257_ExceedsDefaultThreshold) {
    // 257 > kDefaultMaxPackableValue(256) → returns 0 (don't pack)
    std::vector<uint32_t> v = {257};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size()), 0u);
}

// ---------------------------------------------------------------------------
// ComputeMinBitWidth with explicit max_packable_value
// ---------------------------------------------------------------------------

TEST(BitpackCodecTest, MinBitWidth_CustomThreshold_Low) {
    // Threshold = 15 → values up to 15 are packable
    std::vector<uint32_t> v = {0, 5, 10, 15};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size(), 15), 4u);
}

TEST(BitpackCodecTest, MinBitWidth_CustomThreshold_Exceeded) {
    // Threshold = 15, but max_val = 16 → returns 0
    std::vector<uint32_t> v = {1, 2, 16};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size(), 15), 0u);
}

TEST(BitpackCodecTest, MinBitWidth_ThresholdDisabled) {
    // UINT32_MAX disables the check entirely
    std::vector<uint32_t> v = {100000};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size(), UINT32_MAX), 17u);
}

TEST(BitpackCodecTest, MinBitWidth_ThresholdZero_OnlyZerosPass) {
    // Threshold = 0 → only all-zeros is packable
    std::vector<uint32_t> zeros = {0, 0, 0};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(zeros.data(), zeros.size(), 0), 1u);
    std::vector<uint32_t> ones = {0, 1};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(ones.data(), ones.size(), 0), 0u);
}

TEST(BitpackCodecTest, MinBitWidth_ExplicitOverride_LargeValue) {
    // Override with explicit large threshold: 256 → returns 9 bits
    std::vector<uint32_t> v = {256};
    EXPECT_EQ(BitpackCodec::ComputeMinBitWidth(v.data(), v.size(), 1024), 9u);
}

// ---------------------------------------------------------------------------
// EncodedSize
// ---------------------------------------------------------------------------

TEST(BitpackCodecTest, EncodedSize_1Bit_8Values) {
    EXPECT_EQ(BitpackCodec::EncodedSize(8, 1), 1u);
}

TEST(BitpackCodecTest, EncodedSize_1Bit_64Values) {
    EXPECT_EQ(BitpackCodec::EncodedSize(64, 1), 8u);
}

TEST(BitpackCodecTest, EncodedSize_4Bit_8Values) {
    EXPECT_EQ(BitpackCodec::EncodedSize(8, 4), 4u);
}

TEST(BitpackCodecTest, EncodedSize_8Bit_4Values) {
    EXPECT_EQ(BitpackCodec::EncodedSize(4, 8), 4u);
}

TEST(BitpackCodecTest, EncodedSize_RoundsUp) {
    // 3 values × 2 bits = 6 bits → ceil(6/8) = 1 byte
    EXPECT_EQ(BitpackCodec::EncodedSize(3, 2), 1u);
    // 5 values × 2 bits = 10 bits → ceil(10/8) = 2 bytes
    EXPECT_EQ(BitpackCodec::EncodedSize(5, 2), 2u);
}

// ---------------------------------------------------------------------------
// Encode / Decode round-trips
// ---------------------------------------------------------------------------

TEST(BitpackCodecTest, RoundTrip_1Bit_8Values) {
    std::vector<uint32_t> values = {1, 0, 1, 0, 1, 0, 1, 0};
    auto packed = BitpackCodec::Encode(values.data(), values.size(), 1);
    EXPECT_EQ(packed.size(), 1u);  // 8 bits = 1 byte

    auto decoded = BitpackCodec::Decode(packed.data(), 1, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_1Bit_64Values) {
    // 64-element block (primary RaBitQ path)
    std::vector<uint32_t> values(64);
    for (uint32_t i = 0; i < 64; i++) values[i] = i % 2;

    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 1);
    EXPECT_EQ(packed.size(), 8u);

    auto decoded = BitpackCodec::Decode(packed.data(), 1, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_2Bit_4Values) {
    std::vector<uint32_t> values = {0, 1, 2, 3};
    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 2);
    auto decoded = BitpackCodec::Decode(packed.data(), 2, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_4Bit_8Values) {
    std::vector<uint32_t> values = {0, 1, 2, 3, 4, 5, 6, 7};
    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 4);
    EXPECT_EQ(packed.size(), 4u);
    auto decoded = BitpackCodec::Decode(packed.data(), 4, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_8Bit_4Values) {
    std::vector<uint32_t> values = {10, 50, 100, 200};
    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 8);
    auto decoded = BitpackCodec::Decode(packed.data(), 8, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_AllZeros_1Bit) {
    std::vector<uint32_t> values(64, 0u);
    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 1);
    for (uint8_t b : packed) EXPECT_EQ(b, 0u);
    auto decoded = BitpackCodec::Decode(packed.data(), 1, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_AllOnes_1Bit) {
    std::vector<uint32_t> values(64, 1u);
    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 1);
    for (uint8_t b : packed) EXPECT_EQ(b, 0xFFu);
    auto decoded = BitpackCodec::Decode(packed.data(), 1, values.size());
    EXPECT_EQ(decoded, values);
}

TEST(BitpackCodecTest, RoundTrip_MaxBitWidth_2Bit) {
    // All values = 3 (max for 2-bit)
    std::vector<uint32_t> values(16, 3u);
    auto packed  = BitpackCodec::Encode(values.data(), values.size(), 2);
    auto decoded = BitpackCodec::Decode(packed.data(), 2, values.size());
    EXPECT_EQ(decoded, values);
}

// ---------------------------------------------------------------------------
// Encode produces correct raw bytes (spot-check)
// ---------------------------------------------------------------------------

TEST(BitpackCodecTest, Encode_1Bit_KnownByte) {
    // [1,0,1,0,1,0,1,0] LSB-first → byte = 0b01010101 = 0x55
    std::vector<uint32_t> values = {1, 0, 1, 0, 1, 0, 1, 0};
    auto packed = BitpackCodec::Encode(values.data(), values.size(), 1);
    ASSERT_EQ(packed.size(), 1u);
    EXPECT_EQ(packed[0], 0x55u);
}

TEST(BitpackCodecTest, Encode_2Bit_KnownByte) {
    // [0,1,2,3] LSB-first 2-bit: 0b11100100 = 0xE4
    std::vector<uint32_t> values = {0, 1, 2, 3};
    auto packed = BitpackCodec::Encode(values.data(), values.size(), 2);
    ASSERT_EQ(packed.size(), 1u);
    EXPECT_EQ(packed[0], 0xE4u);
}

// ---------------------------------------------------------------------------
// Empty input
// ---------------------------------------------------------------------------

TEST(BitpackCodecTest, EncodeDecodeEmpty) {
    auto packed  = BitpackCodec::Encode(nullptr, 0, 1);
    EXPECT_TRUE(packed.empty());

    auto decoded = BitpackCodec::Decode(nullptr, 1, 0);
    EXPECT_TRUE(decoded.empty());
}
