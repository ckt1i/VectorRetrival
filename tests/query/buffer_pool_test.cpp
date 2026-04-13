#include <gtest/gtest.h>

#include "vdb/query/buffer_pool.h"

using namespace vdb::query;

TEST(BufferPoolTest, AcquireAndRelease) {
    BufferPool pool;

    uint8_t* buf1 = pool.Acquire(128);
    ASSERT_NE(buf1, nullptr);
    EXPECT_EQ(pool.OutstandingCount(), 1u);
    EXPECT_EQ(pool.PoolSize(), 0u);

    pool.Release(buf1);
    EXPECT_EQ(pool.OutstandingCount(), 0u);
    EXPECT_EQ(pool.PoolSize(), 1u);
}

TEST(BufferPoolTest, Reuse) {
    BufferPool pool;

    uint8_t* buf1 = pool.Acquire(128);
    pool.Release(buf1);

    // Should reuse the same buffer (capacity 128 >= 64)
    uint8_t* buf2 = pool.Acquire(64);
    EXPECT_EQ(buf2, buf1);
    pool.Release(buf2);
}

TEST(BufferPoolTest, AllocateNewWhenTooSmall) {
    BufferPool pool;

    uint8_t* buf1 = pool.Acquire(64);
    pool.Release(buf1);

    uint8_t* buf2 = pool.Acquire(8192);
    ASSERT_NE(buf2, nullptr);
    EXPECT_EQ(pool.OutstandingCount(), 1u);

    pool.Release(buf2);
    EXPECT_EQ(pool.OutstandingCount(), 0u);
    EXPECT_GE(pool.PoolSize(), 1u);
}

TEST(BufferPoolTest, MultipleBuffers) {
    BufferPool pool;

    uint8_t* a = pool.Acquire(100);
    uint8_t* b = pool.Acquire(200);
    uint8_t* c = pool.Acquire(300);

    EXPECT_EQ(pool.OutstandingCount(), 3u);

    pool.Release(a);
    pool.Release(b);
    pool.Release(c);

    EXPECT_EQ(pool.OutstandingCount(), 0u);
    EXPECT_EQ(pool.PoolSize(), 3u);
}

TEST(BufferPoolTest, WriteAndReadBack) {
    BufferPool pool;

    uint8_t* buf = pool.Acquire(16);
    for (int i = 0; i < 16; ++i) buf[i] = static_cast<uint8_t>(i);

    pool.Release(buf);
    uint8_t* buf2 = pool.Acquire(16);
    // Reused buffer should still have old data
    EXPECT_EQ(buf2, buf);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(buf2[i], static_cast<uint8_t>(i));
    }
    pool.Release(buf2);
}

TEST(BufferPoolTest, PrimePreallocatesReusableBuffers) {
    BufferPool pool;
    pool.Prime(256, 3);

    EXPECT_EQ(pool.PoolSize(), 3u);
    EXPECT_EQ(pool.OutstandingCount(), 0u);

    uint8_t* a = pool.Acquire(128);
    uint8_t* b = pool.Acquire(128);
    EXPECT_NE(a, nullptr);
    EXPECT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(pool.PoolSize(), 1u);

    pool.Release(a);
    pool.Release(b);
    EXPECT_EQ(pool.PoolSize(), 3u);
}
