# Tasks: io_uring SQ Auto-Flush

## Step 1: Fix PrepRead + Error Handling

- [x] **1.1** Add auto-flush to `IoUringReader::PrepRead`
  - When `io_uring_get_sqe()` returns NULL, call `io_uring_submit()` then retry
  - Update `inflight_` and reset `prepped_` after auto-flush submit
  - File: src/query/io_uring_reader.cpp

- [x] **1.2** Verify `Submit()` handles the case where auto-flush already submitted some SQEs
  - Check that `prepped_` count is correct after mixed auto-flush + explicit Submit
  - Check `inflight_` tracking is consistent

- [x] **1.3** Verify `PreadFallbackReader` has no similar issue
  - pread is synchronous — PrepRead just queues, Submit executes all. No SQ limit.

- [x] **1.4** Add fatal error handling for all PrepRead call sites in overlap_scheduler.cpp
  - 7 call sites (lines 86, 258, 265, 299, 306, 320, 473)
  - Check return value of `reader_.PrepRead()`
  - On failure: `std::fprintf(stderr, ...) + std::abort()` — immediate termination
  - With auto-flush, PrepRead should never fail in practice
  - If it does fail, it means a kernel-level I/O error — continuing would corrupt results

## Step 2: Build and Test

- [x] **2.1** Build and run all existing tests
- [x] **2.2** Run bench_e2e on coco_100k with bits=1, nprobe=nlist, early_stop=0, --queries 100
  - Expect: recall@10 >= 0.99, false_safeout ≈ 0
- [x] **2.3** Run bench_e2e on coco_100k with bits=4
  - Compare with bench_vector_search reference
