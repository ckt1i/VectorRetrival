#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/popcount.h"

using vdb::rabitq::RotationMatrix;
using vdb::rabitq::RaBitQEncoder;
using vdb::rabitq::RaBitQCode;
using vdb::Dim;

namespace {

/// Generate a random float vector in [-1, 1]^dim
std::vector<float> RandomVec(Dim dim, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(dim);
    for (auto& x : v) x = dist(rng);
    return v;
}

/// Compute L2 norm of a vector
float VecNorm(const float* v, Dim d) {
    float s = 0.0f;
    for (Dim i = 0; i < d; ++i) s += v[i] * v[i];
    return std::sqrt(s);
}

}  // namespace

// ===========================================================================
// Basic encoding
// ===========================================================================

TEST(RaBitQEncoderTest, CodeSizeCorrect) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());

    EXPECT_EQ(code.code.size(), static_cast<size_t>(encoder.num_code_words()));
    EXPECT_EQ(encoder.num_code_words(), 2u);  // 128/64 = 2
}

TEST(RaBitQEncoderTest, CodeSize_NonMultipleOf64) {
    Dim dim = 100;  // Not a multiple of 64
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    EXPECT_EQ(encoder.num_code_words(), 2u);  // ceil(100/64) = 2

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());
    EXPECT_EQ(code.code.size(), 2u);
}

TEST(RaBitQEncoderTest, NormStoredCorrectly_NoCentroid) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 200);
    auto code = encoder.Encode(vec.data());

    // Norm should equal ‖vec‖ since centroid is zero
    float expected_norm = VecNorm(vec.data(), dim);
    EXPECT_NEAR(code.norm, expected_norm, 1e-5f);
}

TEST(RaBitQEncoderTest, NormStoredCorrectly_WithCentroid) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 200);
    auto centroid = RandomVec(dim, 300);
    auto code = encoder.Encode(vec.data(), centroid.data());

    // Norm should equal ‖vec - centroid‖
    std::vector<float> residual(dim);
    for (Dim i = 0; i < dim; ++i) residual[i] = vec[i] - centroid[i];
    float expected_norm = VecNorm(residual.data(), dim);
    EXPECT_NEAR(code.norm, expected_norm, 1e-5f);
}

TEST(RaBitQEncoderTest, PopcountConsistent) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 500);
    auto code = encoder.Encode(vec.data());

    // sum_x should match manual popcount
    uint32_t manual_popcount = vdb::simd::PopcountTotal(
        code.code.data(), static_cast<uint32_t>(code.code.size()));
    EXPECT_EQ(code.sum_x, manual_popcount);
}

TEST(RaBitQEncoderTest, BitsAreZeroOrOne) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 600);
    auto code = encoder.Encode(vec.data());

    // sum_x should be between 0 and dim (inclusive)
    EXPECT_GE(code.sum_x, 0u);
    EXPECT_LE(code.sum_x, dim);
}

TEST(RaBitQEncoderTest, SumXApproximatelyHalf) {
    // For random vectors after random rotation, sign bits should be
    // roughly 50/50. We check with a generous margin.
    Dim dim = 256;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    uint32_t total_sum_x = 0;
    const int N = 100;
    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 1000 + i);
        auto code = encoder.Encode(vec.data());
        total_sum_x += code.sum_x;
    }

    // Average sum_x should be around dim/2 = 128
    float avg = static_cast<float>(total_sum_x) / N;
    EXPECT_GT(avg, 90.0f);   // generous lower bound
    EXPECT_LT(avg, 166.0f);  // generous upper bound
}

// ===========================================================================
// Zero vector edge case
// ===========================================================================

TEST(RaBitQEncoderTest, ZeroVector) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    std::vector<float> zero(dim, 0.0f);
    auto code = encoder.Encode(zero.data());

    EXPECT_NEAR(code.norm, 0.0f, 1e-30f);
    // Code bits are implementation-defined for zero vector (all sign(0) >= 0 → 1)
}

// ===========================================================================
// BatchEncode
// ===========================================================================

TEST(RaBitQEncoderTest, BatchEncode_MatchesSingle) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    const uint32_t N = 10;
    std::vector<float> batch(N * dim);
    std::mt19937_64 rng(999);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& x : batch) x = dist(rng);

    auto batch_codes = encoder.EncodeBatch(batch.data(), N);
    EXPECT_EQ(batch_codes.size(), N);

    // Compare with individual encodes
    for (uint32_t i = 0; i < N; ++i) {
        auto single = encoder.Encode(batch.data() + i * dim);
        EXPECT_EQ(batch_codes[i].code, single.code) << "vector " << i;
        EXPECT_FLOAT_EQ(batch_codes[i].norm, single.norm) << "vector " << i;
        EXPECT_EQ(batch_codes[i].sum_x, single.sum_x) << "vector " << i;
    }
}

// ===========================================================================
// Determinism
// ===========================================================================

TEST(RaBitQEncoderTest, DeterministicEncoding) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 777);
    auto code1 = encoder.Encode(vec.data());
    auto code2 = encoder.Encode(vec.data());

    EXPECT_EQ(code1.code, code2.code);
    EXPECT_FLOAT_EQ(code1.norm, code2.norm);
    EXPECT_EQ(code1.sum_x, code2.sum_x);
}

// ===========================================================================
// High bits in tail words should be zero
// ===========================================================================

TEST(RaBitQEncoderTest, TailBitsZero) {
    Dim dim = 100;  // 100 dims → 2 words, but only 36 bits used in word 1
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 888);
    auto code = encoder.Encode(vec.data());

    // Word 1 should have bits 36..63 all zero
    uint64_t tail_mask = ~((1ULL << 36) - 1);  // mask for bits 36-63
    EXPECT_EQ(code.code[1] & tail_mask, 0u);
}

// ===========================================================================
// Hadamard rotation should also work
// ===========================================================================

TEST(RaBitQEncoderTest, HadamardRotation) {
    Dim dim = 64;
    RotationMatrix P(dim);
    ASSERT_TRUE(P.GenerateHadamard(42, true));
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());

    EXPECT_EQ(code.code.size(), 1u);
    EXPECT_GT(code.norm, 0.0f);
    EXPECT_GE(code.sum_x, 0u);
    EXPECT_LE(code.sum_x, dim);
}
