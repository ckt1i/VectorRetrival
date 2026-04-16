#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "vdb/simd/ip_exrabitq.h"

namespace {

float ScalarIPExRaBitQ(const float* query,
                       const uint8_t* code_abs,
                       const uint8_t* sign,
                       uint32_t dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        const float s = sign[i] ? 1.0f : -1.0f;
        sum += query[i] * s * (static_cast<float>(code_abs[i]) + 0.5f);
    }
    return sum;
}

std::vector<float> MakeQuery(uint32_t dim) {
    std::vector<float> out(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        const float sign = (i % 3 == 0) ? -1.0f : 1.0f;
        out[i] = sign * (0.125f * static_cast<float>((i % 17) + 1));
    }
    return out;
}

std::vector<uint8_t> MakeCodeAbs(uint32_t dim) {
    std::vector<uint8_t> out(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        out[i] = static_cast<uint8_t>((i * 11u + 7u) & 0x0F);
    }
    return out;
}

std::vector<uint8_t> MakeSign(uint32_t dim, int mode) {
    std::vector<uint8_t> out(dim, 1u);
    for (uint32_t i = 0; i < dim; ++i) {
        if (mode == 0) {
            out[i] = 1u;
        } else if (mode == 1) {
            out[i] = 0u;
        } else {
            out[i] = static_cast<uint8_t>(((i * 5u) + 3u) & 1u);
        }
    }
    return out;
}

}  // namespace

TEST(IPExRaBitQTest, MatchesScalarAcrossDimsAndSignPatterns) {
    for (uint32_t dim : {16u, 32u, 64u, 70u, 512u}) {
        const auto query = MakeQuery(dim);
        const auto code_abs = MakeCodeAbs(dim);

        for (int sign_mode = 0; sign_mode < 3; ++sign_mode) {
            const auto sign = MakeSign(dim, sign_mode);

            const float actual = vdb::simd::IPExRaBitQ(
                query.data(), code_abs.data(), sign.data(), dim);
            const float expected = ScalarIPExRaBitQ(
                query.data(), code_abs.data(), sign.data(), dim);

            EXPECT_NEAR(actual, expected, 1e-4f)
                << "dim=" << dim << " sign_mode=" << sign_mode;
        }
    }
}
