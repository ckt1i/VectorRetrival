#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "vdb/common/status.h"

namespace vdb {
namespace query {

/// I/O completion result
struct IoCompletion {
    uint8_t* buffer;    // From CQE user_data
    int32_t result;     // Success: bytes read, Failure: negative errno
};

// ============================================================================
// AsyncReader — abstract I/O backend interface
// ============================================================================

/// Maps to io_uring primitives: PrepRead → io_uring_prep_read,
/// Submit → io_uring_submit, Poll/WaitAndPoll → CQE consumption.
class AsyncReader {
 public:
    virtual ~AsyncReader() = default;

    /// Prepare a read (fills SQE, does not submit).
    virtual Status PrepRead(int fd, uint8_t* buf,
                            uint32_t len, uint64_t offset) = 0;

    /// Submit all prepared SQEs.
    /// @return Number of SQEs submitted
    virtual uint32_t Submit() = 0;

    /// Non-blocking poll for completed requests.
    virtual uint32_t Poll(IoCompletion* out, uint32_t max_count) = 0;

    /// Blocking wait for at least one completion, then drain all ready.
    virtual uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) = 0;

    /// Number of in-flight requests.
    virtual uint32_t InFlight() const = 0;
};

// ============================================================================
// PreadFallbackReader — synchronous pread fallback
// ============================================================================

/// Used when io_uring is not available. PrepRead records the request,
/// Submit executes all pending preads synchronously.
class PreadFallbackReader : public AsyncReader {
 public:
    PreadFallbackReader() = default;

    Status PrepRead(int fd, uint8_t* buf,
                    uint32_t len, uint64_t offset) override;
    uint32_t Submit() override;
    uint32_t Poll(IoCompletion* out, uint32_t max_count) override;
    uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) override;
    uint32_t InFlight() const override;

 private:
    struct PendingEntry {
        int fd;
        uint8_t* buf;
        uint32_t len;
        uint64_t offset;
    };

    std::vector<PendingEntry> pending_;
    std::deque<IoCompletion> completed_;
};

// ============================================================================
// IoUringReader — io_uring async I/O (declared here, implemented separately)
// ============================================================================

class IoUringReader : public AsyncReader {
 public:
    IoUringReader();
    ~IoUringReader() override;

    /// Initialize io_uring ring.
    /// @param queue_depth SQ depth
    /// @param cq_entries CQ capacity (via IORING_SETUP_CQSIZE)
    /// @return Status::OK or Status::NotSupported
    Status Init(uint32_t queue_depth = 64, uint32_t cq_entries = 4096,
                bool use_iopoll = false);

    /// Register file descriptors for IOSQE_FIXED_FILE optimization.
    /// After registration, use PrepReadFixed with the fd index (0-based).
    Status RegisterFiles(const int* fds, uint32_t count);

    Status PrepRead(int fd, uint8_t* buf,
                    uint32_t len, uint64_t offset) override;

    /// PrepRead using a registered fd index (IOSQE_FIXED_FILE).
    Status PrepReadFixed(int fd_index, uint8_t* buf,
                         uint32_t len, uint64_t offset);

    uint32_t Submit() override;
    uint32_t Poll(IoCompletion* out, uint32_t max_count) override;
    uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) override;
    uint32_t InFlight() const override;

 private:
    struct Impl;
    Impl* impl_ = nullptr;
    uint32_t in_flight_ = 0;
    uint32_t prepped_ = 0;
};

}  // namespace query
}  // namespace vdb
