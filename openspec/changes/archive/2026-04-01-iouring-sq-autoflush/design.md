# Design: io_uring SQ Auto-Flush

## Current Code

```cpp
// src/query/io_uring_reader.cpp
Status IoUringReader::PrepRead(int fd, uint8_t* buf,
                                uint32_t len, uint64_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        return Status::IOError("io_uring SQ full, cannot prep read");
    }
    io_uring_prep_read(sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(sqe, buf);
    ++prepped_;
    return Status::OK();
}
```

## New Code

```cpp
Status IoUringReader::PrepRead(int fd, uint8_t* buf,
                                uint32_t len, uint64_t offset) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        // SQ full — flush current SQEs to kernel, then retry
        int ret = io_uring_submit(&impl_->ring);
        if (ret < 0) {
            return Status::IOError("io_uring_submit failed during auto-flush");
        }
        inflight_ += prepped_;
        prepped_ = 0;

        sqe = io_uring_get_sqe(&impl_->ring);
        if (!sqe) {
            return Status::IOError("io_uring SQ full after auto-flush");
        }
    }
    io_uring_prep_read(sqe, fd, buf, len, offset);
    io_uring_sqe_set_data(sqe, buf);
    ++prepped_;
    return Status::OK();
}
```

## Key Points

1. **Auto-flush**: When SQ is full, `io_uring_submit()` pushes all pending SQEs to the kernel. This frees SQ slots for new requests.

2. **inflight_ tracking**: After auto-flush, `prepped_` count transfers to `inflight_`. The existing `Submit()` method also does this. Need to verify the Submit implementation handles this correctly.

3. **CQ capacity**: Auto-flush can cause many CQEs to accumulate. CQ capacity is 4096 (set via `IORING_SETUP_CQSIZE`). Max inflight per cluster is ~3125 (coco_100k). Safe margin.

4. **No caller changes**: ProbeCluster and ProbeAndDrainInterleaved remain unchanged. The fix is transparent to the I/O pipeline.

## Error Handling in overlap_scheduler.cpp

All 7 PrepRead call sites abort on failure:

```cpp
// Before (silent failure):
reader_.PrepRead(dat_fd, buf, vec_bytes_, addr.offset);

// After (fatal on error):
auto s = reader_.PrepRead(dat_fd, buf, vec_bytes_, addr.offset);
if (!s.ok()) {
    std::fprintf(stderr, "FATAL: PrepRead failed: %s\n", s.ToString().c_str());
    std::abort();
}
```

With auto-flush, PrepRead failure means a kernel-level I/O error. Continuing would produce garbage rerank results. Abort is the only safe option.

## Files Modified

```
src/query/io_uring_reader.cpp    — PrepRead: add auto-flush
src/query/overlap_scheduler.cpp  — all 7 PrepRead call sites: add fatal error check
```
