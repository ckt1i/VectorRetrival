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
};

IoUringReader::IoUringReader() : impl_(new Impl()) {}

IoUringReader::~IoUringReader() {
    if (impl_->initialized) {
        io_uring_queue_exit(&impl_->ring);
    }
    delete impl_;
}

Status IoUringReader::Init(uint32_t queue_depth, uint32_t cq_entries) {
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = cq_entries;

    int ret = io_uring_queue_init_params(queue_depth, &impl_->ring, &params);
    if (ret < 0) {
        return Status::NotSupported(
            std::string("io_uring init failed: ") + strerror(-ret));
    }
    impl_->initialized = true;
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
    io_uring_prep_read(sqe, fd, buf, len, offset);
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
