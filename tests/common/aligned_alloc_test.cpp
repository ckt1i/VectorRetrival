#include <gtest/gtest.h>
#include "vdb/common/aligned_alloc.h"
#include "vdb/common/macros.h"

namespace vdb {
namespace {

TEST(AlignedAllocTest, BasicAlignment) {
  void* ptr = AlignedAlloc(1024, 64);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64, 0);
  AlignedFree(ptr);
}

TEST(AlignedAllocTest, SimdAlignment) {
  void* ptr = SimdAlloc(256);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % kSimdWidth, 0);
  AlignedFree(ptr);
}

TEST(AlignedAllocTest, CacheLineAlignment) {
  void* ptr = CacheLineAlloc(128);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % kCacheLineSize, 0);
  AlignedFree(ptr);
}

TEST(AlignedAllocTest, BlockAlignment) {
  void* ptr = BlockAlloc(8192);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % kDefaultBlockSize, 0);
  AlignedFree(ptr);
}

TEST(AlignedAllocTest, ZeroSize) {
  void* ptr = AlignedAlloc(0, 64);
  EXPECT_EQ(ptr, nullptr);
}

TEST(AlignedAllocTest, FreeNull) {
  // Should not crash
  AlignedFree(nullptr);
}

// AlignedBuffer tests
TEST(AlignedBufferTest, DefaultConstruction) {
  AlignedBuffer buf;
  EXPECT_EQ(buf.data(), nullptr);
  EXPECT_EQ(buf.size(), 0);
  EXPECT_EQ(buf.capacity(), 0);
  EXPECT_TRUE(buf.empty());
}

TEST(AlignedBufferTest, SizedConstruction) {
  AlignedBuffer buf(1024, 32);
  ASSERT_NE(buf.data(), nullptr);
  EXPECT_EQ(buf.size(), 1024);
  EXPECT_GE(buf.capacity(), 1024);
  EXPECT_EQ(buf.alignment(), 32);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data()) % 32, 0);
}

TEST(AlignedBufferTest, MoveConstruction) {
  AlignedBuffer buf1(512);
  uint8_t* original_ptr = buf1.data();
  
  AlignedBuffer buf2(std::move(buf1));
  EXPECT_EQ(buf2.data(), original_ptr);
  EXPECT_EQ(buf2.size(), 512);
  EXPECT_EQ(buf1.data(), nullptr);
  EXPECT_EQ(buf1.size(), 0);
}

TEST(AlignedBufferTest, MoveAssignment) {
  AlignedBuffer buf1(512);
  AlignedBuffer buf2(256);
  uint8_t* ptr1 = buf1.data();
  
  buf2 = std::move(buf1);
  EXPECT_EQ(buf2.data(), ptr1);
  EXPECT_EQ(buf2.size(), 512);
}

TEST(AlignedBufferTest, ElementAccess) {
  AlignedBuffer buf(100);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<uint8_t>(i);
  }
  for (size_t i = 0; i < buf.size(); ++i) {
    EXPECT_EQ(buf[i], static_cast<uint8_t>(i));
  }
}

TEST(AlignedBufferTest, TypedAccess) {
  AlignedBuffer buf(sizeof(float) * 4, kSimdWidth);
  float* floats = buf.as<float>();
  floats[0] = 1.0f;
  floats[1] = 2.0f;
  floats[2] = 3.0f;
  floats[3] = 4.0f;
  
  const AlignedBuffer& const_buf = buf;
  const float* const_floats = const_buf.as<float>();
  EXPECT_EQ(const_floats[0], 1.0f);
  EXPECT_EQ(const_floats[3], 4.0f);
}

TEST(AlignedBufferTest, Resize) {
  AlignedBuffer buf(100);
  EXPECT_EQ(buf.size(), 100);
  
  buf.Resize(50);  // Shrink
  EXPECT_EQ(buf.size(), 50);
  
  buf.Resize(200);  // Grow
  EXPECT_EQ(buf.size(), 200);
  EXPECT_GE(buf.capacity(), 200);
}

TEST(AlignedBufferTest, Reserve) {
  AlignedBuffer buf(100);
  buf.Reserve(1000);
  
  EXPECT_EQ(buf.size(), 100);  // Size unchanged
  EXPECT_GE(buf.capacity(), 1000);
}

TEST(AlignedBufferTest, Clear) {
  AlignedBuffer buf(100);
  size_t cap = buf.capacity();
  
  buf.Clear();
  EXPECT_EQ(buf.size(), 0);
  EXPECT_EQ(buf.capacity(), cap);  // Capacity preserved
  EXPECT_TRUE(buf.empty());
}

TEST(AlignedBufferTest, Zero) {
  AlignedBuffer buf(100);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = 0xFF;
  }
  
  buf.Zero();
  for (size_t i = 0; i < buf.size(); ++i) {
    EXPECT_EQ(buf[i], 0);
  }
}

// AlignedVector tests
TEST(AlignedVectorTest, BasicUsage) {
  AlignedVector<float> vec;
  vec.push_back(1.0f);
  vec.push_back(2.0f);
  vec.push_back(3.0f);
  
  EXPECT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 1.0f);
  EXPECT_EQ(vec[1], 2.0f);
  EXPECT_EQ(vec[2], 3.0f);
  
  // Check alignment
  EXPECT_EQ(reinterpret_cast<uintptr_t>(vec.data()) % kSimdWidth, 0);
}

TEST(AlignedVectorTest, LargeAllocation) {
  AlignedVector<float> vec(1000, 1.0f);
  EXPECT_EQ(vec.size(), 1000);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(vec.data()) % kSimdWidth, 0);
  
  for (const auto& v : vec) {
    EXPECT_EQ(v, 1.0f);
  }
}

// RoundUp/RoundDown tests
TEST(MacrosTest, RoundUpPow2) {
  EXPECT_EQ(RoundUpPow2(0, 64), 0);
  EXPECT_EQ(RoundUpPow2(1, 64), 64);
  EXPECT_EQ(RoundUpPow2(63, 64), 64);
  EXPECT_EQ(RoundUpPow2(64, 64), 64);
  EXPECT_EQ(RoundUpPow2(65, 64), 128);
  EXPECT_EQ(RoundUpPow2(100, 16), 112);
}

TEST(MacrosTest, RoundDownPow2) {
  EXPECT_EQ(RoundDownPow2(0, 64), 0);
  EXPECT_EQ(RoundDownPow2(1, 64), 0);
  EXPECT_EQ(RoundDownPow2(63, 64), 0);
  EXPECT_EQ(RoundDownPow2(64, 64), 64);
  EXPECT_EQ(RoundDownPow2(65, 64), 64);
  EXPECT_EQ(RoundDownPow2(127, 64), 64);
  EXPECT_EQ(RoundDownPow2(128, 64), 128);
}

}  // namespace
}  // namespace vdb
