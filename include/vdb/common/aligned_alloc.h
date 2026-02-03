#pragma once

#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

#include "vdb/common/types.h"

#ifdef VDB_USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace vdb {

// ============================================================================
// Aligned Memory Allocation
// ============================================================================

/// Allocate aligned memory
/// @param size Number of bytes to allocate
/// @param alignment Alignment requirement (must be power of 2)
/// @return Pointer to aligned memory, or nullptr on failure
inline void* AlignedAlloc(size_t size, size_t alignment) {
  if (size == 0) {
    return nullptr;
  }
  
#ifdef VDB_USE_MIMALLOC
  return mi_aligned_alloc(alignment, size);
#elif defined(_MSC_VER)
  return _aligned_malloc(size, alignment);
#else
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0) {
    return nullptr;
  }
  return ptr;
#endif
}

/// Free aligned memory
inline void AlignedFree(void* ptr) {
  if (ptr == nullptr) {
    return;
  }
  
#ifdef VDB_USE_MIMALLOC
  mi_free(ptr);
#elif defined(_MSC_VER)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

/// Allocate memory aligned to SIMD width (32 bytes for AVX2)
inline void* SimdAlloc(size_t size) {
  return AlignedAlloc(size, kSimdWidth);
}

/// Allocate memory aligned to cache line (64 bytes)
inline void* CacheLineAlloc(size_t size) {
  return AlignedAlloc(size, kCacheLineSize);
}

/// Allocate memory aligned to block size (4KB)
inline void* BlockAlloc(size_t size) {
  return AlignedAlloc(size, kDefaultBlockSize);
}

// ============================================================================
// Aligned Unique Pointer
// ============================================================================

/// Custom deleter for aligned memory
struct AlignedDeleter {
  void operator()(void* ptr) const {
    AlignedFree(ptr);
  }
};

/// Unique pointer with aligned memory
template <typename T>
using AlignedUniquePtr = std::unique_ptr<T, AlignedDeleter>;

/// Create aligned unique pointer for single object
template <typename T, typename... Args>
AlignedUniquePtr<T> MakeAligned(size_t alignment, Args&&... args) {
  void* ptr = AlignedAlloc(sizeof(T), alignment);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  try {
    return AlignedUniquePtr<T>(new (ptr) T(std::forward<Args>(args)...));
  } catch (...) {
    AlignedFree(ptr);
    throw;
  }
}

/// Create SIMD-aligned unique pointer
template <typename T, typename... Args>
AlignedUniquePtr<T> MakeSimdAligned(Args&&... args) {
  return MakeAligned<T>(kSimdWidth, std::forward<Args>(args)...);
}

// ============================================================================
// Aligned Buffer (for raw byte arrays)
// ============================================================================

class AlignedBuffer {
 public:
  AlignedBuffer() : data_(nullptr), size_(0), capacity_(0), alignment_(0) {}
  
  explicit AlignedBuffer(size_t size, size_t alignment = kSimdWidth)
      : size_(size), capacity_(size), alignment_(alignment) {
    if (size > 0) {
      data_ = static_cast<uint8_t*>(AlignedAlloc(size, alignment));
      if (data_ == nullptr) {
        throw std::bad_alloc();
      }
    } else {
      data_ = nullptr;
    }
  }
  
  // Non-copyable
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  
  // Movable
  AlignedBuffer(AlignedBuffer&& other) noexcept
      : data_(other.data_),
        size_(other.size_),
        capacity_(other.capacity_),
        alignment_(other.alignment_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }
  
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
    if (this != &other) {
      AlignedFree(data_);
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      alignment_ = other.alignment_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }
  
  ~AlignedBuffer() {
    AlignedFree(data_);
  }
  
  // Accessors
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  size_t alignment() const { return alignment_; }
  bool empty() const { return size_ == 0; }
  
  // Element access
  uint8_t& operator[](size_t i) { return data_[i]; }
  const uint8_t& operator[](size_t i) const { return data_[i]; }
  
  // Typed access
  template <typename T>
  T* as() { return reinterpret_cast<T*>(data_); }
  
  template <typename T>
  const T* as() const { return reinterpret_cast<const T*>(data_); }
  
  /// Resize buffer (may reallocate)
  void Resize(size_t new_size) {
    if (new_size <= capacity_) {
      size_ = new_size;
      return;
    }
    
    // Grow with some slack
    size_t new_capacity = std::max(new_size, capacity_ * 2);
    uint8_t* new_data = static_cast<uint8_t*>(
        AlignedAlloc(new_capacity, alignment_ > 0 ? alignment_ : kSimdWidth));
    if (new_data == nullptr) {
      throw std::bad_alloc();
    }
    
    if (data_ != nullptr) {
      std::memcpy(new_data, data_, size_);
      AlignedFree(data_);
    }
    
    data_ = new_data;
    size_ = new_size;
    capacity_ = new_capacity;
  }
  
  /// Reserve capacity without changing size
  void Reserve(size_t new_capacity) {
    if (new_capacity <= capacity_) {
      return;
    }
    
    uint8_t* new_data = static_cast<uint8_t*>(
        AlignedAlloc(new_capacity, alignment_ > 0 ? alignment_ : kSimdWidth));
    if (new_data == nullptr) {
      throw std::bad_alloc();
    }
    
    if (data_ != nullptr) {
      std::memcpy(new_data, data_, size_);
      AlignedFree(data_);
    }
    
    data_ = new_data;
    capacity_ = new_capacity;
  }
  
  /// Clear buffer (set size to 0, keep capacity)
  void Clear() {
    size_ = 0;
  }
  
  /// Zero-fill the buffer
  void Zero() {
    if (data_ != nullptr) {
      std::memset(data_, 0, size_);
    }
  }
  
 private:
  uint8_t* data_;
  size_t size_;
  size_t capacity_;
  size_t alignment_;
};

// ============================================================================
// Aligned STL Allocator
// ============================================================================

template <typename T, size_t Alignment = kSimdWidth>
class AlignedAllocator {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using propagate_on_container_move_assignment = std::true_type;
  using is_always_equal = std::true_type;
  
  constexpr AlignedAllocator() noexcept = default;
  
  template <typename U>
  constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}
  
  [[nodiscard]] T* allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    
    void* ptr = AlignedAlloc(n * sizeof(T), Alignment);
    if (ptr == nullptr) {
      throw std::bad_alloc();
    }
    return static_cast<T*>(ptr);
  }
  
  void deallocate(T* ptr, std::size_t) noexcept {
    AlignedFree(ptr);
  }
  
  template <typename U>
  struct rebind {
    using other = AlignedAllocator<U, Alignment>;
  };
};

template <typename T1, typename T2, size_t A1, size_t A2>
bool operator==(const AlignedAllocator<T1, A1>&,
                const AlignedAllocator<T2, A2>&) noexcept {
  return A1 == A2;
}

template <typename T1, typename T2, size_t A1, size_t A2>
bool operator!=(const AlignedAllocator<T1, A1>&,
                const AlignedAllocator<T2, A2>&) noexcept {
  return A1 != A2;
}

// ============================================================================
// Type aliases for common aligned vectors
// ============================================================================

#include <vector>

/// SIMD-aligned vector
template <typename T>
using AlignedVector = std::vector<T, AlignedAllocator<T, kSimdWidth>>;

/// Cache-line aligned vector
template <typename T>
using CacheAlignedVector = std::vector<T, AlignedAllocator<T, kCacheLineSize>>;

}  // namespace vdb
