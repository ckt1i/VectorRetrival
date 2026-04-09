#include <gtest/gtest.h>
#include <cstdint>
#include <cmath>
#include <random>
#include <vector>

#include "vdb/simd/signed_dot.h"

namespace {

// Scalar reference — matches the original loop from bench_vector_search.cpp
static float ScalarRef(const float* q, const uint64_t* sign_bits,
                       uint32_t dim) {
    float dot = 0.0f;
    for (uint32_t d = 0; d < dim; ++d) {
        int bit = static_cast<int>((sign_bits[d / 64] >> (d % 64)) & 1);
        dot += q[d] * (2.0f * static_cast<float>(bit) - 1.0f);
    }
    return dot;
}

// Build a packed uint64_t bit array from a bool vector
static std::vector<uint64_t> PackBits(const std::vector<bool>& bits) {
    uint32_t num_words = (static_cast<uint32_t>(bits.size()) + 63) / 64;
    std::vector<uint64_t> words(num_words, 0ULL);
    for (uint32_t i = 0; i < static_cast<uint32_t>(bits.size()); ++i) {
        if (bits[i]) words[i / 64] |= (1ULL << (i % 64));
    }
    return words;
}

}  // namespace

// ============================================================================
// ScalarEquivalence — SIMD result matches scalar for random q + random bits
// ============================================================================
class SignedDotTest : public ::testing::TestWithParam<uint32_t> {};

TEST_P(SignedDotTest, ScalarEquivalence) {
    const uint32_t dim = GetParam();
    std::mt19937_64 rng(42u + dim);
    std::uniform_real_distribution<float> qdist(-2.0f, 2.0f);
    std::bernoulli_distribution bdist(0.5);

    std::vector<float> q(dim);
    std::vector<bool> bit_vec(dim);
    for (uint32_t d = 0; d < dim; ++d) {
        q[d] = qdist(rng);
        bit_vec[d] = bdist(rng);
    }
    auto sign_bits = PackBits(bit_vec);

    float expected = ScalarRef(q.data(), sign_bits.data(), dim);
    float actual   = vdb::simd::SignedDotFromBits(q.data(), sign_bits.data(), dim);

    if (std::abs(expected) > 1e-6f) {
        EXPECT_NEAR(actual, expected, std::abs(expected) * 1e-4f)
            << "dim=" << dim;
    } else {
        EXPECT_NEAR(actual, expected, 1e-5f) << "dim=" << dim;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Dims, SignedDotTest,
    ::testing::Values(64u, 96u, 128u, 256u, 512u));

// ============================================================================
// AllZeroBits — all bits 0 → each s(i) = -1 → result = -Σq[i]
// ============================================================================
TEST(SignedDotFromBitsTest, AllZeroBits) {
    const uint32_t dim = 128;
    std::vector<float> q(dim, 1.0f);  // all ones
    std::vector<uint64_t> sign_bits((dim + 63) / 64, 0ULL);

    float result = vdb::simd::SignedDotFromBits(q.data(), sign_bits.data(), dim);
    EXPECT_NEAR(result, -static_cast<float>(dim), 1e-4f);
}

// ============================================================================
// AllOneBits — all bits 1 → each s(i) = +1 → result = +Σq[i]
// ============================================================================
TEST(SignedDotFromBitsTest, AllOneBits) {
    const uint32_t dim = 128;
    std::vector<float> q(dim, 1.0f);
    std::vector<uint64_t> sign_bits((dim + 63) / 64, ~0ULL);

    float result = vdb::simd::SignedDotFromBits(q.data(), sign_bits.data(), dim);
    EXPECT_NEAR(result, static_cast<float>(dim), 1e-4f);
}

// ============================================================================
// PaddedDim — non-multiple of 16 (dim=96) tail handled correctly
// ============================================================================
TEST(SignedDotFromBitsTest, PaddedDim) {
    const uint32_t dim = 96;
    std::mt19937_64 rng(1234u);
    std::uniform_real_distribution<float> qdist(-1.0f, 1.0f);
    std::bernoulli_distribution bdist(0.5);

    std::vector<float> q(dim);
    std::vector<bool> bv(dim);
    for (uint32_t d = 0; d < dim; ++d) {
        q[d] = qdist(rng);
        bv[d] = bdist(rng);
    }
    auto sign_bits = PackBits(bv);

    float expected = ScalarRef(q.data(), sign_bits.data(), dim);
    float actual   = vdb::simd::SignedDotFromBits(q.data(), sign_bits.data(), dim);

    EXPECT_NEAR(actual, expected, std::abs(expected) * 1e-4f + 1e-5f);
}
