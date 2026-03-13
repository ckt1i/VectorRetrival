#include <gtest/gtest.h>
#include <cstdint>
#include <numeric>
#include <vector>

#include "vdb/simd/transpose.h"

using vdb::simd::Transpose8xN;
using vdb::simd::TransposeNx8;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build K streams of length N with deterministic values: stream[k][j] = k*1000+j
static std::vector<std::vector<uint32_t>> MakeStreams(uint32_t K, uint32_t N) {
    std::vector<std::vector<uint32_t>> streams(K, std::vector<uint32_t>(N));
    for (uint32_t k = 0; k < K; ++k) {
        for (uint32_t j = 0; j < N; ++j) {
            streams[k][j] = k * 1000 + j;
        }
    }
    return streams;
}

/// Collect raw pointers for the API
static std::vector<const uint32_t*> ConstPtrs(
    const std::vector<std::vector<uint32_t>>& streams) {
    std::vector<const uint32_t*> ptrs(streams.size());
    for (size_t i = 0; i < streams.size(); ++i) {
        ptrs[i] = streams[i].data();
    }
    return ptrs;
}

static std::vector<uint32_t*> MutPtrs(
    std::vector<std::vector<uint32_t>>& streams) {
    std::vector<uint32_t*> ptrs(streams.size());
    for (size_t i = 0; i < streams.size(); ++i) {
        ptrs[i] = streams[i].data();
    }
    return ptrs;
}

// ---------------------------------------------------------------------------
// Transpose8xN basic correctness
// ---------------------------------------------------------------------------

class Transpose8xNTest : public ::testing::TestWithParam<
    std::tuple<uint32_t /*num_streams*/, uint32_t /*count*/>> {};

TEST_P(Transpose8xNTest, Correctness) {
    auto [K, N] = GetParam();
    auto streams = MakeStreams(K, N);
    auto ptrs = ConstPtrs(streams);

    std::vector<uint32_t> interleaved(N * 8, 0xDEADBEEF);
    Transpose8xN(ptrs.data(), interleaved.data(), K, N);

    for (uint32_t j = 0; j < N; ++j) {
        for (uint32_t k = 0; k < K; ++k) {
            EXPECT_EQ(interleaved[j * 8 + k], streams[k][j])
                << "at j=" << j << " k=" << k;
        }
        // Excess lanes must be zero
        for (uint32_t k = K; k < 8; ++k) {
            EXPECT_EQ(interleaved[j * 8 + k], 0u)
                << "excess lane at j=" << j << " k=" << k;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    TransposeParams, Transpose8xNTest,
    ::testing::Combine(
        ::testing::Values(1, 2, 3, 4, 5, 7, 8),   // num_streams
        ::testing::Values(1, 4, 7, 8, 9, 16, 63, 64, 65, 128)  // count
    ));

// ---------------------------------------------------------------------------
// TransposeNx8 basic correctness
// ---------------------------------------------------------------------------

class TransposeNx8Test : public ::testing::TestWithParam<
    std::tuple<uint32_t /*num_streams*/, uint32_t /*count*/>> {};

TEST_P(TransposeNx8Test, Correctness) {
    auto [K, N] = GetParam();
    auto streams = MakeStreams(K, N);
    auto ptrs = ConstPtrs(streams);

    // First transpose SoA → interleaved
    std::vector<uint32_t> interleaved(N * 8, 0);
    Transpose8xN(ptrs.data(), interleaved.data(), K, N);

    // Then inverse: interleaved → SoA
    std::vector<std::vector<uint32_t>> out(K, std::vector<uint32_t>(N, 0xDEADBEEF));
    auto out_ptrs = MutPtrs(out);
    TransposeNx8(interleaved.data(), out_ptrs.data(), K, N);

    for (uint32_t k = 0; k < K; ++k) {
        for (uint32_t j = 0; j < N; ++j) {
            EXPECT_EQ(out[k][j], streams[k][j])
                << "at k=" << k << " j=" << j;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    TransposeParams, TransposeNx8Test,
    ::testing::Combine(
        ::testing::Values(1, 2, 4, 8),
        ::testing::Values(1, 7, 8, 9, 64, 65)
    ));

// ---------------------------------------------------------------------------
// Roundtrip: Transpose8xN → TransposeNx8 recovers original
// ---------------------------------------------------------------------------

TEST(TransposeRoundtripTest, FullWidth8x64) {
    const uint32_t K = 8, N = 64;
    auto streams = MakeStreams(K, N);
    auto ptrs = ConstPtrs(streams);

    std::vector<uint32_t> interleaved(N * 8);
    Transpose8xN(ptrs.data(), interleaved.data(), K, N);

    std::vector<std::vector<uint32_t>> recovered(K, std::vector<uint32_t>(N));
    auto rec_ptrs = MutPtrs(recovered);
    TransposeNx8(interleaved.data(), rec_ptrs.data(), K, N);

    for (uint32_t k = 0; k < K; ++k) {
        EXPECT_EQ(recovered[k], streams[k]) << "stream " << k << " differs";
    }
}

TEST(TransposeRoundtripTest, PartialWidth3x17) {
    const uint32_t K = 3, N = 17;
    auto streams = MakeStreams(K, N);
    auto ptrs = ConstPtrs(streams);

    std::vector<uint32_t> interleaved(N * 8);
    Transpose8xN(ptrs.data(), interleaved.data(), K, N);

    std::vector<std::vector<uint32_t>> recovered(K, std::vector<uint32_t>(N));
    auto rec_ptrs = MutPtrs(recovered);
    TransposeNx8(interleaved.data(), rec_ptrs.data(), K, N);

    for (uint32_t k = 0; k < K; ++k) {
        EXPECT_EQ(recovered[k], streams[k]) << "stream " << k << " differs";
    }
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(TransposeEdgeTest, ZeroCount) {
    // Must not crash
    Transpose8xN(nullptr, nullptr, 8, 0);
    TransposeNx8(nullptr, nullptr, 8, 0);
}

TEST(TransposeEdgeTest, ZeroStreams) {
    Transpose8xN(nullptr, nullptr, 0, 64);
    TransposeNx8(nullptr, nullptr, 0, 64);
}
