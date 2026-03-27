#include "vdb/index/crc_stopper.h"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace vdb {
namespace index {
namespace {

// Helper to make CalibrationResults with sensible defaults.
CalibrationResults MakeParams(float lamhat, float d_min = 0.0f,
                              float d_max = 10.0f, float reg_lambda = 0.01f,
                              uint32_t kreg = 1) {
    return {lamhat, kreg, reg_lambda, d_min, d_max};
}

TEST(CrcStopperTest, HeapNotFull_NeverStops) {
    // When the heap is not full, current_kth_dist = +inf.
    // nonconf clamps to 1.0, (1 - nonconf) = 0, reg_score is tiny → no stop.
    auto params = MakeParams(0.5f);
    CrcStopper stopper(params, 16);
    float inf = std::numeric_limits<float>::infinity();

    for (uint32_t p = 1; p <= 16; ++p) {
        EXPECT_FALSE(stopper.ShouldStop(p, inf))
            << "Should not stop when heap is not full, probed=" << p;
    }
}

TEST(CrcStopperTest, DmaxEqualsDmin_NosCrash) {
    // d_max == d_min → d_range_inv = 0, nonconf = 0, treated as best quality.
    CalibrationResults params{0.5f, 1, 0.01f, 5.0f, 5.0f};
    CrcStopper stopper(params, 16);

    // Should not crash. With nonconf=0, reg_score = 1/max_reg_val + penalty,
    // which may or may not exceed lamhat depending on probed_count.
    EXPECT_NO_FATAL_FAILURE(stopper.ShouldStop(1, 5.0f));
    EXPECT_NO_FATAL_FAILURE(stopper.ShouldStop(16, 5.0f));
}

TEST(CrcStopperTest, LamhatZero_AlwaysStopsWhenHeapFull) {
    // lamhat = 0 → any reg_score > 0 triggers stop.
    // When heap is full (finite dist), (1-nonconf) > 0, so reg_score > 0.
    auto params = MakeParams(0.0f, 0.0f, 10.0f);
    CrcStopper stopper(params, 16);

    EXPECT_TRUE(stopper.ShouldStop(1, 5.0f));
    EXPECT_TRUE(stopper.ShouldStop(1, 0.0f));  // best possible distance
}

TEST(CrcStopperTest, LamhatOne_NeverStops) {
    // lamhat = 1.0 → reg_score / max_reg_val < 1.0 always (due to *1.2 margin).
    auto params = MakeParams(1.0f, 0.0f, 10.0f, 0.01f, 1);
    CrcStopper stopper(params, 16);

    EXPECT_FALSE(stopper.ShouldStop(1, 0.0f));
    EXPECT_FALSE(stopper.ShouldStop(8, 3.0f));
    EXPECT_FALSE(stopper.ShouldStop(16, 0.0f));
}

TEST(CrcStopperTest, RegScoreIncreasesWithProbedCount) {
    // With fixed current_kth_dist, increasing probed_count adds more penalty,
    // so reg_score should be non-decreasing. Eventually it should trigger stop.
    auto params = MakeParams(0.5f, 0.0f, 10.0f, 0.1f, 1);
    CrcStopper stopper(params, 20);

    bool found_stop = false;
    uint32_t stop_at = 0;
    for (uint32_t p = 1; p <= 20; ++p) {
        if (stopper.ShouldStop(p, 3.0f)) {
            found_stop = true;
            stop_at = p;
            break;
        }
    }
    EXPECT_TRUE(found_stop) << "Should eventually stop as probed_count increases";

    // After the stop point, all subsequent probes should also stop
    // (since penalty only increases).
    for (uint32_t p = stop_at; p <= 20; ++p) {
        EXPECT_TRUE(stopper.ShouldStop(p, 3.0f))
            << "Should still stop at probed=" << p;
    }
}

TEST(CrcStopperTest, BetterDistanceStopsSooner) {
    // A smaller current_kth_dist (better quality) → smaller nonconf →
    // larger (1-nonconf) → larger reg_score → stops sooner.
    auto params = MakeParams(0.4f, 0.0f, 10.0f, 0.01f, 1);
    CrcStopper stopper(params, 16);

    // Find stop point for good distance (1.0) and bad distance (9.0).
    auto find_stop = [&](float dist) -> uint32_t {
        for (uint32_t p = 1; p <= 16; ++p) {
            if (stopper.ShouldStop(p, dist)) return p;
        }
        return 17;  // didn't stop
    };

    uint32_t stop_good = find_stop(1.0f);
    uint32_t stop_bad = find_stop(9.0f);
    EXPECT_LE(stop_good, stop_bad)
        << "Better distance should stop no later than worse distance";
}

TEST(CrcStopperTest, DefaultConstructor_DoesNotCrash) {
    CrcStopper stopper;
    // Default-constructed stopper should not crash when called.
    EXPECT_NO_FATAL_FAILURE(stopper.ShouldStop(1, 5.0f));
}

}  // namespace
}  // namespace index
}  // namespace vdb
