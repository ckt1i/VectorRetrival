#include <gtest/gtest.h>

#include "vdb/query/result_collector.h"

using namespace vdb;
using namespace vdb::query;

TEST(ResultCollectorTest, BasicTopK) {
    ResultCollector collector(3);

    EXPECT_FALSE(collector.Full());
    EXPECT_EQ(collector.Size(), 0u);

    EXPECT_TRUE(collector.TryInsert(5.0f, {100, 10}));
    EXPECT_TRUE(collector.TryInsert(3.0f, {200, 20}));
    EXPECT_TRUE(collector.TryInsert(7.0f, {300, 30}));
    EXPECT_TRUE(collector.Full());

    // 7.0 is the worst; inserting 2.0 should replace it
    EXPECT_FLOAT_EQ(collector.TopDistance(), 7.0f);
    EXPECT_TRUE(collector.TryInsert(2.0f, {400, 40}));
    EXPECT_FLOAT_EQ(collector.TopDistance(), 5.0f);

    // Inserting 10.0 should be rejected
    EXPECT_FALSE(collector.TryInsert(10.0f, {500, 50}));
}

TEST(ResultCollectorTest, FinalizeReturnsAscending) {
    ResultCollector collector(5);

    collector.TryInsert(5.0f, {50, 1});
    collector.TryInsert(1.0f, {10, 1});
    collector.TryInsert(3.0f, {30, 1});
    collector.TryInsert(4.0f, {40, 1});
    collector.TryInsert(2.0f, {20, 1});

    auto results = collector.Finalize();
    ASSERT_EQ(results.size(), 5u);

    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].distance, results[i].distance);
    }
    EXPECT_FLOAT_EQ(results[0].distance, 1.0f);
    EXPECT_FLOAT_EQ(results[4].distance, 5.0f);
}

TEST(ResultCollectorTest, LessThanTopK) {
    ResultCollector collector(10);

    collector.TryInsert(3.0f, {30, 1});
    collector.TryInsert(1.0f, {10, 1});

    EXPECT_FALSE(collector.Full());
    EXPECT_EQ(collector.Size(), 2u);

    auto results = collector.Finalize();
    ASSERT_EQ(results.size(), 2u);
    EXPECT_FLOAT_EQ(results[0].distance, 1.0f);
    EXPECT_FLOAT_EQ(results[1].distance, 3.0f);
}

TEST(ResultCollectorTest, TopDistanceWhenEmpty) {
    ResultCollector collector(5);
    EXPECT_GT(collector.TopDistance(), 1e30f);
}
