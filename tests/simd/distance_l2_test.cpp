#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "vdb/simd/distance_l2.h"

using vdb::simd::L2Sqr;

// ---------------------------------------------------------------------------
// Basic correctness
// ---------------------------------------------------------------------------

TEST(DistanceL2Test, ZeroVectors) {
    float a[4] = {0.f, 0.f, 0.f, 0.f};
    float b[4] = {0.f, 0.f, 0.f, 0.f};
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 4), 0.f);
}

TEST(DistanceL2Test, SameVector) {
    float a[8] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
    EXPECT_FLOAT_EQ(L2Sqr(a, a, 8), 0.f);
}

TEST(DistanceL2Test, ZeroDimension) {
    float a[1] = {1.f};
    float b[1] = {2.f};
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 0), 0.f);
}

TEST(DistanceL2Test, SingleDimension) {
    float a[1] = {3.f};
    float b[1] = {7.f};
    // (3-7)^2 = 16
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 1), 16.f);
}

TEST(DistanceL2Test, ThreeDim_KnownValues) {
    float a[3] = {1.f, 2.f, 3.f};
    float b[3] = {4.f, 5.f, 6.f};
    // (1-4)^2 + (2-5)^2 + (3-6)^2 = 9+9+9 = 27
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 3), 27.f);
}

TEST(DistanceL2Test, Symmetry) {
    float a[5] = {1.f, 0.f, -1.f, 2.f, -2.f};
    float b[5] = {0.f, 1.f,  0.f, 0.f,  1.f};
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 5), L2Sqr(b, a, 5));
}

// ---------------------------------------------------------------------------
// AVX2 boundary: exact multiples of 8 and non-multiples
// ---------------------------------------------------------------------------

TEST(DistanceL2Test, Dim8_ExactAVX2Batch) {
    float a[8] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
    float b[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    // 8 × 1^2 = 8
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 8), 8.f);
}

TEST(DistanceL2Test, Dim9_OneTailElement) {
    // 8 zeros and a 1 in position 8 (the scalar tail)
    std::vector<float> a(9, 0.f), b(9, 0.f);
    a[8] = 3.f;
    // distance = 3^2 = 9
    EXPECT_FLOAT_EQ(L2Sqr(a.data(), b.data(), 9), 9.f);
}

TEST(DistanceL2Test, Dim16_TwoBatches) {
    // Two identical 8-element batches: each contributes 8 × 1^2 = 8
    std::vector<float> a(16, 1.f), b(16, 0.f);
    EXPECT_FLOAT_EQ(L2Sqr(a.data(), b.data(), 16), 16.f);
}

TEST(DistanceL2Test, Dim13_MixedBatchAndTail) {
    // [1,1,...,1] vs [0,...,0], 13 dims → 13
    std::vector<float> a(13, 1.f), b(13, 0.f);
    EXPECT_FLOAT_EQ(L2Sqr(a.data(), b.data(), 13), 13.f);
}

TEST(DistanceL2Test, Dim128_LargeVector) {
    // All-ones vs all-zeros: result == 128
    std::vector<float> a(128, 1.f), b(128, 0.f);
    EXPECT_FLOAT_EQ(L2Sqr(a.data(), b.data(), 128), 128.f);
}

// ---------------------------------------------------------------------------
// Numerical: negative values, non-unit distances
// ---------------------------------------------------------------------------

TEST(DistanceL2Test, NegativeValues) {
    float a[4] = {-1.f, -2.f, -3.f, -4.f};
    float b[4] = { 1.f,  2.f,  3.f,  4.f};
    // (-1-1)^2 + (-2-2)^2 + (-3-3)^2 + (-4-4)^2 = 4+16+36+64 = 120
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 4), 120.f);
}

TEST(DistanceL2Test, FloatingPointPrecision) {
    // Use values that sum neatly in float32
    float a[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float b[4] = {1.5f, 1.5f, 1.5f, 1.5f};
    // 4 × (0.5-1.5)^2 = 4 × 1.0 = 4.0
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 4), 4.f);
}
