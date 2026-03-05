#include <gtest/gtest.h>
#include <vector>
#include <string>

#include "vdb/codec/dict_codec.h"

using vdb::codec::DictCodec;
using vdb::codec::DictCodecResult;

// ---------------------------------------------------------------------------
// BuildDict — basic behavior
// ---------------------------------------------------------------------------

TEST(DictCodecTest, EmptyInput) {
    auto result = DictCodec::BuildDict({});
    EXPECT_TRUE(result.dict.empty());
    EXPECT_TRUE(result.indices.empty());
}

TEST(DictCodecTest, SingleElement) {
    auto result = DictCodec::BuildDict({"hello"});
    ASSERT_EQ(result.dict.size(), 1u);
    EXPECT_EQ(result.dict[0], "hello");
    ASSERT_EQ(result.indices.size(), 1u);
    EXPECT_EQ(result.indices[0], 0u);
}

TEST(DictCodecTest, AllUnique) {
    auto result = DictCodec::BuildDict({"apple", "banana", "cherry"});
    EXPECT_EQ(result.dict.size(), 3u);
    EXPECT_EQ(result.indices.size(), 3u);
    // Insertion order preserved
    EXPECT_EQ(result.dict[0], "apple");
    EXPECT_EQ(result.dict[1], "banana");
    EXPECT_EQ(result.dict[2], "cherry");
    EXPECT_EQ(result.indices[0], 0u);
    EXPECT_EQ(result.indices[1], 1u);
    EXPECT_EQ(result.indices[2], 2u);
}

TEST(DictCodecTest, AllDuplicates) {
    auto result = DictCodec::BuildDict({"x", "x", "x", "x"});
    ASSERT_EQ(result.dict.size(), 1u);
    EXPECT_EQ(result.dict[0], "x");
    EXPECT_EQ(result.indices, (std::vector<uint32_t>{0, 0, 0, 0}));
}

TEST(DictCodecTest, MixedUniquesAndDuplicates) {
    // "a" appears at positions 0, 2, 4; "b" at 1, 3; "c" at 5
    auto result = DictCodec::BuildDict({"a", "b", "a", "b", "a", "c"});
    // dict should be in insertion order: ["a","b","c"]
    ASSERT_EQ(result.dict.size(), 3u);
    EXPECT_EQ(result.dict[0], "a");
    EXPECT_EQ(result.dict[1], "b");
    EXPECT_EQ(result.dict[2], "c");
    EXPECT_EQ(result.indices, (std::vector<uint32_t>{0, 1, 0, 1, 0, 2}));
}

TEST(DictCodecTest, InsertionOrderPreserved) {
    // First occurrence wins; order defined by first appearance
    auto result = DictCodec::BuildDict({"z", "a", "m", "a", "z"});
    ASSERT_EQ(result.dict.size(), 3u);
    EXPECT_EQ(result.dict[0], "z");
    EXPECT_EQ(result.dict[1], "a");
    EXPECT_EQ(result.dict[2], "m");
}

TEST(DictCodecTest, EmptyStringValue) {
    auto result = DictCodec::BuildDict({"", "hello", "", ""});
    ASSERT_EQ(result.dict.size(), 2u);
    EXPECT_EQ(result.dict[0], "");
    EXPECT_EQ(result.dict[1], "hello");
    EXPECT_EQ(result.indices, (std::vector<uint32_t>{0, 1, 0, 0}));
}

TEST(DictCodecTest, UnicodeStrings) {
    auto result = DictCodec::BuildDict({"日本語", "中文", "日本語"});
    ASSERT_EQ(result.dict.size(), 2u);
    EXPECT_EQ(result.dict[0], "日本語");
    EXPECT_EQ(result.dict[1], "中文");
    EXPECT_EQ(result.indices, (std::vector<uint32_t>{0, 1, 0}));
}

// ---------------------------------------------------------------------------
// Decode — basic behavior
// ---------------------------------------------------------------------------

TEST(DictCodecTest, Decode_Empty) {
    auto result = DictCodec::Decode({}, {"a", "b"});
    EXPECT_TRUE(result.empty());
}

TEST(DictCodecTest, Decode_SingleElement) {
    auto result = DictCodec::Decode({1u}, {"a", "b", "c"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "b");
}

TEST(DictCodecTest, Decode_KnownValues) {
    std::vector<std::string> dict = {"cat", "dog", "bird"};
    std::vector<uint32_t> indices = {2, 0, 1, 0, 2};
    auto result = DictCodec::Decode(indices, dict);
    EXPECT_EQ(result, (std::vector<std::string>{"bird", "cat", "dog", "cat", "bird"}));
}

TEST(DictCodecTest, Decode_OutOfBoundsIndex_ReturnsEmpty) {
    // Out-of-range index → empty string (graceful degradation)
    std::vector<std::string> dict = {"a"};
    auto result = DictCodec::Decode({0u, 99u}, dict);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "a");
    EXPECT_EQ(result[1], "");
}

// ---------------------------------------------------------------------------
// Round-trip: BuildDict then Decode
// ---------------------------------------------------------------------------

TEST(DictCodecTest, RoundTrip_Basic) {
    std::vector<std::string> input = {"red", "green", "blue", "red", "green"};
    auto encoded = DictCodec::BuildDict(input);
    auto decoded = DictCodec::Decode(encoded.indices, encoded.dict);
    EXPECT_EQ(decoded, input);
}

TEST(DictCodecTest, RoundTrip_AllSame) {
    std::vector<std::string> input(10, "same");
    auto encoded = DictCodec::BuildDict(input);
    auto decoded = DictCodec::Decode(encoded.indices, encoded.dict);
    EXPECT_EQ(decoded, input);
}

TEST(DictCodecTest, RoundTrip_AllUnique) {
    std::vector<std::string> input = {"one", "two", "three", "four", "five"};
    auto encoded = DictCodec::BuildDict(input);
    EXPECT_EQ(encoded.dict.size(), input.size());
    auto decoded = DictCodec::Decode(encoded.indices, encoded.dict);
    EXPECT_EQ(decoded, input);
}

TEST(DictCodecTest, RoundTrip_LargeInput) {
    // 1000 elements cycling over 10 unique strings
    std::vector<std::string> categories = {
        "alpha","beta","gamma","delta","epsilon",
        "zeta","eta","theta","iota","kappa"
    };
    std::vector<std::string> input;
    input.reserve(1000);
    for (int i = 0; i < 1000; i++) {
        input.push_back(categories[i % 10]);
    }

    auto encoded = DictCodec::BuildDict(input);
    EXPECT_EQ(encoded.dict.size(), 10u);
    EXPECT_EQ(encoded.indices.size(), 1000u);

    auto decoded = DictCodec::Decode(encoded.indices, encoded.dict);
    EXPECT_EQ(decoded, input);
}

// ---------------------------------------------------------------------------
// Integration: index compression with ComputeMinBitWidth
// ---------------------------------------------------------------------------

TEST(DictCodecTest, IndexFitsIn4Bits_For16UniqueValues) {
    // 16 unique strings → indices ∈ [0,15] → 4 bits per index
    std::vector<std::string> input;
    for (int i = 0; i < 16; i++) input.push_back("v" + std::to_string(i));
    // Repeat to create a larger array
    auto twice = input;
    twice.insert(twice.end(), input.begin(), input.end());

    auto encoded = DictCodec::BuildDict(twice);
    EXPECT_EQ(encoded.dict.size(), 16u);

    // All indices fit in 4 bits (0..15)
    for (uint32_t idx : encoded.indices) {
        EXPECT_LT(idx, 16u);
    }
}
