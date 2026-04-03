#include "vdb/query/async_reader.h"

#include <liburing.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace vdb {
namespace query {

struct IoUringReader::Impl {
    struct io_uring ring;
    bool initialized = false;
    bool defer_taskrun = false;
    bool files_registered = false;
    // fd → registered index mapping (small, typically 2 entries)
    static constexpr int kMaxRegisteredFds = 8;
    int registered_fds[kMaxRegisteredFds] = {};
    uint32_t num_registered = 0;

    int FindFdIndex(int fd) const {
        for (uint32_t i = 0; i < num_registered; ++i) {
            if (registered_fds[i] == fd) return static_cast<int>(i);
        }
        return -1;
    }
};

IoUringReader::IoUringReader() : impl_(new Impl()) {}

IoUringReader::~IoUringReader() {
    if (impl_->initialized) {
        io_uring_queue_exit(&impl_->ring);
    }
    delete impl_;
}

Status IoUringReader::Init(uint32_t queue_depth, uint32_t cq_entries,
                            bool use_iopoll) {
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_CQSIZE
                 | IORING_SETUP_SINGLE_ISSUER
                 | IORING_SETUP_DEFER_TASKRUN;
    if (use_iopoll) params.flags |= IORING_SETUP_IOPOLL;
    params.cq_entries = cq_entries;

    int ret = io_uring_queue_init_params(queue_depth, &impl_->ring, &params);
    if (ret < 0) {
        // Fallback: kernel may not support DEFER_TASKRUN / SINGLE_ISSUER.
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_CQSIZE;
        params.cq_entries = cq_entries;
        ret = io_uring_queue_init_params(queue_depth, &impl_->ring, &params);
        if (ret < 0) {
            return Status::NotSupported(
                std::string("io_uring init failed: ") + strerror(-ret));
        }
        impl_->defer_taskrun = false;
    } else {
        impl_->defer_taskrun = true;
    }
    impl_->initialized = true;
    return Status::OK();
}

Status IoUringReader::RegisterFiles(const int* fds, uint32_t count) {
    if (count > Impl::kMaxRegisteredFds) {
        return Status::InvalidArgument("Too many fds to register");
    }
    int ret = io_uring_register_files(&impl_->ring, fds, count);
    if (ret < 0) {
        return Status::IOError(
            std::string("io_uring_register_files failed: ") + strerror(-ret));
    }
    for (uint32_t i = 0; i < count; ++i) {
        impl_->registered_fds[i] = fds[i];
    }
    impl_->num_registered = count;
    impl_->files_registered = true;
    return Status::OK();
}

Status IoUringReader::PrepReadFixed(int fd_index, uint8_t* buf,
                                     uint32_t len, uint64_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        int ret = io_uring_submit(&impl_->ring);
        if (ret < 0) {
            return Status::IOError(
                std::string("io_uring auto-flush submit failed: ") +
                strerror(-ret));
        }
        in_flight_ += prepped_;
        prepped_ = 0;

        sqe = io_uring_get_sqe(&impl_->ring);
        if (!sqe) {
            return Status::IOError("io_uring SQ full after auto-flush");
        }
    }
    io_uring_prep_read(sqe, fd_index, buf, len, offset);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, buf);
    ++prepped_;
    return Status::OK();
}

Status IoUringReader::PrepRead(int fd, uint8_t* buf,
                                uint32_t len, uint64_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        // SQ full — flush current SQEs to kernel, then retry.
        int ret = io_uring_submit(&impl_->ring);
        if (ret < 0) {
            return Status::IOError(
                std::string("io_uring auto-flush submit failed: ") +
                strerror(-ret));
        }
        in_flight_ += prepped_;
        prepped_ = 0;

        sqe = io_uring_get_sqe(&impl_->ring);
        if (!sqe) {
            return Status::IOError(
                "io_uring SQ full after auto-flush");
        }
    }
    // Automatically use FIXED_FILE if this fd was registered
    int fd_idx = impl_->files_registered ? impl_->FindFdIndex(fd) : -1;
    if (fd_idx >= 0) {
        io_uring_prep_read(sqe, fd_idx, buf, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    } else {
        io_uring_prep_read(sqe, fd, buf, len, offset);
    }
    io_uring_sqe_set_data(sqe, buf);
    ++prepped_;
    return Status::OK();
}

uint32_t IoUringReader::Submit() {
    if (prepped_ == 0) return 0;

    int ret = io_uring_submit(&impl_->ring);
    uint32_t submitted = (ret > 0) ? static_cast<uint32_t>(ret) : 0;
    in_flight_ += submitted;
    prepped_ = 0;
    return submitted;
}

uint32_t IoUringReader::Poll(IoCompletion* out, uint32_t max_count) {
    // Under DEFER_TASKRUN, CQEs won't appear until we explicitly ask the
    // kernel to run deferred task_work.  io_uring_get_events() triggers this
    // without blocking.
    if (impl_->defer_taskrun) {
        io_uring_get_events(&impl_->ring);
    }

    uint32_t count = 0;
    struct io_uring_cqe* cqe;

    while (count < max_count) {
        int ret = io_uring_peek_cqe(&impl_->ring, &cqe);
        if (ret != 0) break;  // No more completions

        out[count].buffer = static_cast<uint8_t*>(io_uring_cqe_get_data(cqe));
        out[count].result = cqe->res;
        io_uring_cqe_seen(&impl_->ring, cqe);
        ++count;
        --in_flight_;
    }
    return count;
}

uint32_t IoUringReader::WaitAndPoll(IoCompletion* out, uint32_t max_count) {
    if (max_count == 0 || in_flight_ == 0) return 0;

    // Wait for at least one completion
    struct io_uring_cqe* cqe;
    int ret = io_uring_wait_cqe(&impl_->ring, &cqe);
    if (ret < 0) return 0;

    // Consume this one
    out[0].buffer = static_cast<uint8_t*>(io_uring_cqe_get_data(cqe));
    out[0].result = cqe->res;
    io_uring_cqe_seen(&impl_->ring, cqe);
    --in_flight_;

    // Drain any additional ready completions
    uint32_t count = 1;
    while (count < max_count) {
        ret = io_uring_peek_cqe(&impl_->ring, &cqe);
        if (ret != 0) break;

        out[count].buffer = static_cast<uint8_t*>(io_uring_cqe_get_data(cqe));
        out[count].result = cqe->res;
        io_uring_cqe_seen(&impl_->ring, cqe);
        ++count;
        --in_flight_;
    }
    return count;
}

uint32_t IoUringReader::InFlight() const {
    return in_flight_;
}

}  // namespace query
}  // namespace vdb
