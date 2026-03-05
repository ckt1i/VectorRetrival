#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "vdb/simd/prefix_sum.h"

using vdb::simd::ExclusivePrefixSum;

// Helper: compute exclusive prefix sum into a new vector
static std::vector<uint32_t> PrefixSum(const std::vector<uint32_t>& in) {
    std::vector<uint32_t> out(in.size());
    if (!in.empty()) {
        ExclusivePrefixSum(in.data(), out.data(), static_cast<uint32_t>(in.size()));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(PrefixSumTest, ZeroCount) {
    // Must not crash or touch any memory
    ExclusivePrefixSum(nullptr, nullptr, 0);
}

TEST(PrefixSumTest, SingleElement) {
    auto result = PrefixSum({42u});
    // Exclusive: out[0] = 0 always
    EXPECT_EQ(result, (std::vector<uint32_t>{0u}));
}

TEST(PrefixSumTest, TwoElements) {
    auto result = PrefixSum({3u, 7u});
    EXPECT_EQ(result, (std::vector<uint32_t>{0u, 3u}));
}

// ---------------------------------------------------------------------------
// All-zeros input
// ---------------------------------------------------------------------------

TEST(PrefixSumTest, AllZeros_4Elements) {
    auto result = PrefixSum({0, 0, 0, 0});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 0, 0, 0}));
}

TEST(PrefixSumTest, AllZeros_64Elements) {
    std::vector<uint32_t> in(64, 0u);
    std::vector<uint32_t> expected(64, 0u);
    auto result = PrefixSum(in);
    EXPECT_EQ(result, expected);
}

// ---------------------------------------------------------------------------
// All-ones: out[i] = i
// ---------------------------------------------------------------------------

TEST(PrefixSumTest, AllOnes_8Elements) {
    auto result = PrefixSum({1, 1, 1, 1, 1, 1, 1, 1});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7}));
}

TEST(PrefixSumTest, AllOnes_64Elements) {
    // Primary use case: 64-element AddressBlock
    std::vector<uint32_t> in(64, 1u);
    std::vector<uint32_t> expected(64);
    std::iota(expected.begin(), expected.end(), 0u);  // 0,1,2,...,63
    auto result = PrefixSum(in);
    EXPECT_EQ(result, expected);
}

// ---------------------------------------------------------------------------
// Known values
// ---------------------------------------------------------------------------

TEST(PrefixSumTest, KnownValues_8Elements) {
    // in = [1,2,3,4,5,6,7,8]
    // out = [0,1,3,6,10,15,21,28]
    auto result = PrefixSum({1, 2, 3, 4, 5, 6, 7, 8});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 3, 6, 10, 15, 21, 28}));
}

TEST(PrefixSumTest, KnownValues_5Elements_ScalarTail) {
    // count=5 exercises the scalar tail when AVX2 is active (5 % 8 = 5)
    auto result = PrefixSum({10, 20, 30, 40, 50});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 10, 30, 60, 100}));
}

TEST(PrefixSumTest, KnownValues_9Elements_OneTailAfterBatch) {
    // 8 (one AVX2 batch) + 1 tail element
    auto result = PrefixSum({1, 1, 1, 1, 1, 1, 1, 1, 100});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(PrefixSumTest, AlternatingValues_8Elements) {
    // in = [1,2,1,2,1,2,1,2]
    // out = [0,1,3,4,6,7,9,10]
    auto result = PrefixSum({1, 2, 1, 2, 1, 2, 1, 2});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1, 3, 4, 6, 7, 9, 10}));
}

// ---------------------------------------------------------------------------
// AVX2 cross-lane carry: test values that exercise the inter-lane addition
// ---------------------------------------------------------------------------

TEST(PrefixSumTest, LargeSingleValue_CrossLane) {
    // First element very large → subsequent elements must carry it
    auto result = PrefixSum({1000u, 1u, 1u, 1u, 1u, 1u, 1u, 1u});
    EXPECT_EQ(result, (std::vector<uint32_t>{0, 1000, 1001, 1002, 1003, 1004, 1005, 1006}));
}

TEST(PrefixSumTest, LaneCarry_16Elements) {
    // 16 elements: 2 AVX2 batches; lane 1 of batch 0 must carry into batch 1
    std::vector<uint32_t> in = {100,100,100,100, 100,100,100,100,
                                  1,  1,  1,  1,   1,  1,  1,  1};
    std::vector<uint32_t> expected;
    uint32_t running = 0;
    for (uint32_t v : in) { expected.push_back(running); running += v; }
    auto result = PrefixSum(in);
    EXPECT_EQ(result, expected);
}

// ---------------------------------------------------------------------------
// 64-element full block (primary production use case)
// ---------------------------------------------------------------------------

TEST(PrefixSumTest, FullBlock64_VerifyTotal) {
    // Build a 64-element block with varied sizes; verify that the exclusive
    // prefix sum out[64] (== running sum after last element) equals total.
    // Since count=64, we just check out[63] + in[63] == sum(in).
    std::vector<uint32_t> in(64);
    for (uint32_t i = 0; i < 64; i++) in[i] = (i % 7) + 1u;  // 1..7, cyclic

    auto result = PrefixSum(in);

    // Verify definition: out[0]=0, out[i] = in[0]+...+in[i-1]
    ASSERT_EQ(result.size(), 64u);
    uint32_t cumsum = 0;
    for (uint32_t i = 0; i < 64u; i++) {
        EXPECT_EQ(result[i], cumsum) << "at i=" << i;
        cumsum += in[i];
    }
}
