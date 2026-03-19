#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "vdb/common/distance.h"

using namespace vdb;

TEST(DistanceTest, IdenticalVectors) {
    float a[] = {1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_FLOAT_EQ(L2Sqr(a, a, 4), 0.0f);
}

TEST(DistanceTest, KnownDistance) {
    float a[] = {1.0f, 0.0f, 0.0f};
    float b[] = {0.0f, 1.0f, 0.0f};
    // (1-0)^2 + (0-1)^2 + (0-0)^2 = 2.0
    EXPECT_FLOAT_EQ(L2Sqr(a, b, 3), 2.0f);
}

TEST(DistanceTest, HigherDim) {
    const Dim dim = 128;
    std::vector<float> a(dim, 1.0f);
    std::vector<float> b(dim, 0.0f);
    // sum((1-0)^2) * 128 = 128.0
    EXPECT_FLOAT_EQ(L2Sqr(a.data(), b.data(), dim), 128.0f);
}

TEST(DistanceTest, NonAlignedDim) {
    // dim not multiple of 8, exercises scalar tail
    const Dim dim = 13;
    std::vector<float> a(dim, 2.0f);
    std::vector<float> b(dim, 0.0f);
    // sum(4.0) * 13 = 52.0
    EXPECT_FLOAT_EQ(L2Sqr(a.data(), b.data(), dim), 52.0f);
}
