#pragma once

#include <cstddef>

namespace vdb {

// ============================================================================
// Compiler Detection
// ============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define VDB_COMPILER_GCC_LIKE 1
#endif

#if defined(_MSC_VER)
#define VDB_COMPILER_MSVC 1
#endif

// ============================================================================
// Branch Prediction Hints
// ============================================================================

#ifdef VDB_COMPILER_GCC_LIKE
#define VDB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define VDB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define VDB_LIKELY(x)   (x)
#define VDB_UNLIKELY(x) (x)
#endif

// ============================================================================
// Force Inline
// ============================================================================

#ifdef VDB_COMPILER_GCC_LIKE
#define VDB_FORCE_INLINE inline __attribute__((always_inline))
#elif defined(VDB_COMPILER_MSVC)
#define VDB_FORCE_INLINE __forceinline
#else
#define VDB_FORCE_INLINE inline
#endif

// ============================================================================
// No Inline
// ============================================================================

#ifdef VDB_COMPILER_GCC_LIKE
#define VDB_NOINLINE __attribute__((noinline))
#elif defined(VDB_COMPILER_MSVC)
#define VDB_NOINLINE __declspec(noinline)
#else
#define VDB_NOINLINE
#endif

// ============================================================================
// Prefetch Hints
// ============================================================================

#ifdef VDB_COMPILER_GCC_LIKE
// Locality hint: 0 = no temporal locality (non-temporal), 3 = high locality
#define VDB_PREFETCH(addr)       __builtin_prefetch(addr, 0, 3)
#define VDB_PREFETCH_READ(addr)  __builtin_prefetch(addr, 0, 3)
#define VDB_PREFETCH_WRITE(addr) __builtin_prefetch(addr, 1, 3)
#define VDB_PREFETCH_NTA(addr)   __builtin_prefetch(addr, 0, 0)  // Non-temporal
#else
#define VDB_PREFETCH(addr)       ((void)0)
#define VDB_PREFETCH_READ(addr)  ((void)0)
#define VDB_PREFETCH_WRITE(addr) ((void)0)
#define VDB_PREFETCH_NTA(addr)   ((void)0)
#endif

// ============================================================================
// Memory Alignment
// ============================================================================

#ifdef VDB_COMPILER_GCC_LIKE
#define VDB_ALIGNED(n) __attribute__((aligned(n)))
#elif defined(VDB_COMPILER_MSVC)
#define VDB_ALIGNED(n) __declspec(align(n))
#else
#define VDB_ALIGNED(n)
#endif

#define VDB_CACHE_ALIGNED VDB_ALIGNED(64)
#define VDB_SIMD_ALIGNED  VDB_ALIGNED(32)

// ============================================================================
// Restrict Pointer (no aliasing)
// ============================================================================

#ifdef VDB_COMPILER_GCC_LIKE
#define VDB_RESTRICT __restrict__
#elif defined(VDB_COMPILER_MSVC)
#define VDB_RESTRICT __restrict
#else
#define VDB_RESTRICT
#endif

// ============================================================================
// Unused Variable/Parameter
// ============================================================================

#define VDB_UNUSED(x) ((void)(x))

// ============================================================================
// Disable Copy
// ============================================================================

#define VDB_DISALLOW_COPY(TypeName)       \
  TypeName(const TypeName&) = delete;     \
  TypeName& operator=(const TypeName&) = delete

// ============================================================================
// Disable Copy and Move
// ============================================================================

#define VDB_DISALLOW_COPY_AND_MOVE(TypeName)  \
  TypeName(const TypeName&) = delete;         \
  TypeName& operator=(const TypeName&) = delete; \
  TypeName(TypeName&&) = delete;              \
  TypeName& operator=(TypeName&&) = delete

// ============================================================================
// Round Up/Down to Power of 2
// ============================================================================

constexpr size_t RoundUpPow2(size_t n, size_t alignment) {
  return (n + alignment - 1) & ~(alignment - 1);
}

constexpr size_t RoundDownPow2(size_t n, size_t alignment) {
  return n & ~(alignment - 1);
}

// ============================================================================
// Compile-time Assertions
// ============================================================================

#define VDB_STATIC_ASSERT(cond, msg) static_assert(cond, msg)

// ============================================================================
// Debug Assertions
// ============================================================================

#ifdef NDEBUG
#define VDB_DCHECK(cond) ((void)0)
#define VDB_DCHECK_EQ(a, b) ((void)0)
#define VDB_DCHECK_NE(a, b) ((void)0)
#define VDB_DCHECK_LT(a, b) ((void)0)
#define VDB_DCHECK_LE(a, b) ((void)0)
#define VDB_DCHECK_GT(a, b) ((void)0)
#define VDB_DCHECK_GE(a, b) ((void)0)
#else
#include <cassert>
#define VDB_DCHECK(cond) assert(cond)
#define VDB_DCHECK_EQ(a, b) assert((a) == (b))
#define VDB_DCHECK_NE(a, b) assert((a) != (b))
#define VDB_DCHECK_LT(a, b) assert((a) < (b))
#define VDB_DCHECK_LE(a, b) assert((a) <= (b))
#define VDB_DCHECK_GT(a, b) assert((a) > (b))
#define VDB_DCHECK_GE(a, b) assert((a) >= (b))
#endif

}  // namespace vdb
