#include <gtest/gtest.h>
#include <numeric>
#include <vector>

#include "vdb/simd/prefix_sum.h"
#include "vdb/simd/transpose.h"

using vdb::simd::ExclusivePrefixSum;
using vdb::simd::ExclusivePrefixSumMulti;
using vdb::simd::Transpose8xN;
using vdb::simd::TransposeNx8;

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

// ===========================================================================
// Multi-stream prefix sum tests
// ===========================================================================

// Helper: compute multi-stream prefix sum via single-stream reference
static void ReferenceMultiPrefixSum(
    const std::vector<std::vector<uint32_t>>& streams,
    std::vector<std::vector<uint32_t>>& out_streams) {
    out_streams.resize(streams.size());
    for (size_t k = 0; k < streams.size(); ++k) {
        out_streams[k].resize(streams[k].size());
        ExclusivePrefixSum(streams[k].data(), out_streams[k].data(),
                           static_cast<uint32_t>(streams[k].size()));
    }
}

class PrefixSumMultiTest : public ::testing::TestWithParam<
    std::tuple<uint32_t /*num_streams*/, uint32_t /*count*/>> {};

TEST_P(PrefixSumMultiTest, ConsistentWithSingleStream) {
    auto [K, N] = GetParam();

    // Build K streams with deterministic values
    std::vector<std::vector<uint32_t>> streams(K, std::vector<uint32_t>(N));
    for (uint32_t k = 0; k < K; ++k) {
        for (uint32_t j = 0; j < N; ++j) {
            streams[k][j] = (k + 1) * (j % 7 + 1);
        }
    }

    // Reference: single-stream prefix sum for each stream
    std::vector<std::vector<uint32_t>> ref_out;
    ReferenceMultiPrefixSum(streams, ref_out);

    // Multi-stream: transpose → ExclusivePrefixSumMulti → transpose back
    std::vector<const uint32_t*> in_ptrs(K);
    for (uint32_t k = 0; k < K; ++k) in_ptrs[k] = streams[k].data();

    std::vector<uint32_t> interleaved_in(N * 8, 0);
    Transpose8xN(in_ptrs.data(), interleaved_in.data(), K, N);

    std::vector<uint32_t> interleaved_out(N * 8, 0);
    ExclusivePrefixSumMulti(interleaved_in.data(), interleaved_out.data(), N, K);

    std::vector<std::vector<uint32_t>> multi_out(K, std::vector<uint32_t>(N, 0));
    std::vector<uint32_t*> out_ptrs(K);
    for (uint32_t k = 0; k < K; ++k) out_ptrs[k] = multi_out[k].data();
    TransposeNx8(interleaved_out.data(), out_ptrs.data(), K, N);

    // Compare
    for (uint32_t k = 0; k < K; ++k) {
        for (uint32_t j = 0; j < N; ++j) {
            EXPECT_EQ(multi_out[k][j], ref_out[k][j])
                << "stream=" << k << " elem=" << j;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    MultiStreamParams, PrefixSumMultiTest,
    ::testing::Combine(
        ::testing::Values(1, 2, 4, 8),
        ::testing::Values(1, 7, 8, 9, 16, 64, 65)
    ));

TEST(PrefixSumMultiTest, ZeroCount) {
    ExclusivePrefixSumMulti(nullptr, nullptr, 0, 8);
}

TEST(PrefixSumMultiTest, ZeroStreams) {
    ExclusivePrefixSumMulti(nullptr, nullptr, 64, 0);
}

TEST(PrefixSumMultiTest, AllZeros_8Streams) {
    const uint32_t N = 16, K = 8;
    std::vector<uint32_t> in(N * 8, 0);
    std::vector<uint32_t> out(N * 8, 0xDEAD);
    ExclusivePrefixSumMulti(in.data(), out.data(), N, K);
    for (uint32_t i = 0; i < N * 8; ++i) {
        EXPECT_EQ(out[i], 0u) << "at index " << i;
    }
}
