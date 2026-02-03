#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <cassert>

namespace vdb {

// ============================================================================
// Status Code
// ============================================================================

enum class StatusCode : uint8_t {
  kOk = 0,
  kInvalidArgument = 1,
  kNotFound = 2,
  kAlreadyExists = 3,
  kIOError = 4,
  kCorruption = 5,
  kNotSupported = 6,
  kOutOfMemory = 7,
  kInternal = 8,
  kCancelled = 9,
  kTimeout = 10,
};

inline std::string_view StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk: return "OK";
    case StatusCode::kInvalidArgument: return "InvalidArgument";
    case StatusCode::kNotFound: return "NotFound";
    case StatusCode::kAlreadyExists: return "AlreadyExists";
    case StatusCode::kIOError: return "IOError";
    case StatusCode::kCorruption: return "Corruption";
    case StatusCode::kNotSupported: return "NotSupported";
    case StatusCode::kOutOfMemory: return "OutOfMemory";
    case StatusCode::kInternal: return "Internal";
    case StatusCode::kCancelled: return "Cancelled";
    case StatusCode::kTimeout: return "Timeout";
    default: return "Unknown";
  }
}

// ============================================================================
// Status Class
// ============================================================================

class Status {
 public:
  // Default constructor creates OK status
  Status() noexcept : state_(nullptr) {}
  
  // Copy constructor
  Status(const Status& other) {
    state_ = (other.state_ == nullptr) ? nullptr : CopyState(other.state_);
  }
  
  // Move constructor
  Status(Status&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
  }
  
  // Copy assignment
  Status& operator=(const Status& other) {
    if (this != &other) {
      delete[] state_;
      state_ = (other.state_ == nullptr) ? nullptr : CopyState(other.state_);
    }
    return *this;
  }
  
  // Move assignment
  Status& operator=(Status&& other) noexcept {
    if (this != &other) {
      delete[] state_;
      state_ = other.state_;
      other.state_ = nullptr;
    }
    return *this;
  }
  
  ~Status() { delete[] state_; }
  
  // Factory methods
  static Status OK() { return Status(); }
  
  static Status InvalidArgument(std::string_view msg) {
    return Status(StatusCode::kInvalidArgument, msg);
  }
  
  static Status NotFound(std::string_view msg) {
    return Status(StatusCode::kNotFound, msg);
  }
  
  static Status AlreadyExists(std::string_view msg) {
    return Status(StatusCode::kAlreadyExists, msg);
  }
  
  static Status IOError(std::string_view msg) {
    return Status(StatusCode::kIOError, msg);
  }
  
  static Status Corruption(std::string_view msg) {
    return Status(StatusCode::kCorruption, msg);
  }
  
  static Status NotSupported(std::string_view msg) {
    return Status(StatusCode::kNotSupported, msg);
  }
  
  static Status OutOfMemory(std::string_view msg) {
    return Status(StatusCode::kOutOfMemory, msg);
  }
  
  static Status Internal(std::string_view msg) {
    return Status(StatusCode::kInternal, msg);
  }
  
  static Status Cancelled(std::string_view msg) {
    return Status(StatusCode::kCancelled, msg);
  }
  
  static Status Timeout(std::string_view msg) {
    return Status(StatusCode::kTimeout, msg);
  }
  
  // Status checks
  bool ok() const { return state_ == nullptr; }
  bool IsInvalidArgument() const { return code() == StatusCode::kInvalidArgument; }
  bool IsNotFound() const { return code() == StatusCode::kNotFound; }
  bool IsAlreadyExists() const { return code() == StatusCode::kAlreadyExists; }
  bool IsIOError() const { return code() == StatusCode::kIOError; }
  bool IsCorruption() const { return code() == StatusCode::kCorruption; }
  bool IsNotSupported() const { return code() == StatusCode::kNotSupported; }
  bool IsOutOfMemory() const { return code() == StatusCode::kOutOfMemory; }
  bool IsInternal() const { return code() == StatusCode::kInternal; }
  bool IsCancelled() const { return code() == StatusCode::kCancelled; }
  bool IsTimeout() const { return code() == StatusCode::kTimeout; }
  
  // Get status code
  StatusCode code() const {
    return (state_ == nullptr) ? StatusCode::kOk 
                               : static_cast<StatusCode>(state_[4]);
  }
  
  // Get error message
  std::string_view message() const {
    if (state_ == nullptr) {
      return std::string_view();
    }
    uint32_t length;
    std::memcpy(&length, state_, sizeof(length));
    return std::string_view(state_ + 5, length);
  }
  
  // Convert to string
  std::string ToString() const {
    if (state_ == nullptr) {
      return "OK";
    }
    std::string result(StatusCodeName(code()));
    std::string_view msg = message();
    if (!msg.empty()) {
      result += ": ";
      result += msg;
    }
    return result;
  }
  
 private:
  // State layout:
  // state_[0..3] = message length (uint32_t)
  // state_[4]    = code (uint8_t)
  // state_[5..]  = message
  const char* state_;
  
  Status(StatusCode code, std::string_view msg) {
    assert(code != StatusCode::kOk);
    const uint32_t length = static_cast<uint32_t>(msg.size());
    char* result = new char[length + 5];
    std::memcpy(result, &length, sizeof(length));
    result[4] = static_cast<char>(code);
    std::memcpy(result + 5, msg.data(), length);
    state_ = result;
  }
  
  static const char* CopyState(const char* state) {
    uint32_t length;
    std::memcpy(&length, state, sizeof(length));
    char* result = new char[length + 5];
    std::memcpy(result, state, length + 5);
    return result;
  }
};

// ============================================================================
// Macros for Error Handling
// ============================================================================

#define VDB_RETURN_IF_ERROR(expr)   \
  do {                              \
    ::vdb::Status _status = (expr); \
    if (!_status.ok()) {            \
      return _status;               \
    }                               \
  } while (0)

#define VDB_CHECK(cond, msg)                            \
  do {                                                  \
    if (!(cond)) {                                      \
      return ::vdb::Status::InvalidArgument(msg);       \
    }                                                   \
  } while (0)

#define VDB_CHECK_NOT_NULL(ptr, msg)                    \
  do {                                                  \
    if ((ptr) == nullptr) {                             \
      return ::vdb::Status::InvalidArgument(msg);       \
    }                                                   \
  } while (0)

// ============================================================================
// StatusOr<T> - A value or an error status
// ============================================================================

template <typename T>
class StatusOr {
 public:
  // Construct from value
  StatusOr(const T& value) : value_(value), status_(Status::OK()) {}
  StatusOr(T&& value) : value_(std::move(value)), status_(Status::OK()) {}
  
  // Construct from status (must not be OK)
  StatusOr(const Status& status) : status_(status) {
    assert(!status.ok());
  }
  StatusOr(Status&& status) : status_(std::move(status)) {
    assert(!status_.ok());
  }
  
  // Copy/move
  StatusOr(const StatusOr&) = default;
  StatusOr(StatusOr&&) = default;
  StatusOr& operator=(const StatusOr&) = default;
  StatusOr& operator=(StatusOr&&) = default;
  
  // Status checks
  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }
  
  // Value access (must be ok())
  const T& value() const & {
    assert(ok());
    return value_;
  }
  
  T& value() & {
    assert(ok());
    return value_;
  }
  
  T&& value() && {
    assert(ok());
    return std::move(value_);
  }
  
  // Convenience operators
  const T& operator*() const & { return value(); }
  T& operator*() & { return value(); }
  T&& operator*() && { return std::move(value()); }
  
  const T* operator->() const { return &value(); }
  T* operator->() { return &value(); }
  
  // Bool conversion
  explicit operator bool() const { return ok(); }
  
 private:
  T value_;
  Status status_;
};

// Specialization for void would go here if needed

#define VDB_ASSIGN_OR_RETURN(lhs, expr)       \
  VDB_ASSIGN_OR_RETURN_IMPL(                  \
      VDB_STATUS_CONCAT(_status_or_, __LINE__), lhs, expr)

#define VDB_ASSIGN_OR_RETURN_IMPL(statusor, lhs, expr) \
  auto statusor = (expr);                              \
  if (!statusor.ok()) {                                \
    return statusor.status();                          \
  }                                                    \
  lhs = std::move(*statusor)

#define VDB_STATUS_CONCAT(x, y) VDB_STATUS_CONCAT_IMPL(x, y)
#define VDB_STATUS_CONCAT_IMPL(x, y) x##y

}  // namespace vdb
