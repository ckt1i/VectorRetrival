# Proposal: io_uring SQ Auto-Flush

## Summary

Fix a silent data corruption bug where `IoUringReader::PrepRead` fails when the SQ ring is full, but the error is ignored by callers. Add auto-flush: when `io_uring_get_sqe()` returns NULL, submit the current SQ to the kernel and retry.

## Problem

`OverlapScheduler::ProbeCluster()` issues one `PrepRead` per Uncertain vector in a cluster. For large clusters (e.g., coco_100k with ~3125 vectors/cluster, ~2000 Uncertain), this exceeds the SQ ring capacity (default 64 slots).

```
PrepRead #1..#64   → SQE slots 0..63  ✓
PrepRead #65..#2000 → io_uring_get_sqe() returns NULL
                    → PrepRead returns IOError
                    → caller ignores error
                    → buffer never filled by kernel
                    → RerankConsumer computes L2 on garbage data
                    → wrong vectors enter top-K → recall collapses
```

Observed: coco_1k (~31 vectors/cluster < 64) works perfectly (recall=0.998). coco_100k (~3125/cluster >> 64) has recall=0.05 even with all classification disabled.

## Fix

Two layers:

1. **Auto-flush**: In `IoUringReader::PrepRead`, when `io_uring_get_sqe()` returns NULL, call `io_uring_submit()` to flush the current SQ to the kernel, then retry. The completed CQEs accumulate in the CQ ring (capacity 4096) and are consumed later by `WaitAndPoll`.

2. **Fatal error handling**: All 7 PrepRead call sites in overlap_scheduler.cpp currently ignore the return value. Add error checks that abort the program on failure. With auto-flush, PrepRead should never fail — if it does, it indicates a kernel-level I/O error where continuing would silently corrupt results. Abort is the only safe response.

## Scope

### In scope
- `IoUringReader::PrepRead` — add auto-flush on SQ full
- `PreadFallbackReader::PrepRead` — verify no similar issue (pread is synchronous, no SQ)

### Out of scope
- ProbeCluster restructuring
- Multi-threaded producer-consumer model
- CQ overflow handling (CQ=4096, max inflight per cluster ~3125, safe)

## Success Criteria

- bench_e2e on coco_100k with bits=1, nprobe=nlist, early_stop=0: recall@10 >= 0.99
- bench_e2e on coco_100k with bits=4: recall@10 comparable to bench_vector_search
- No SQ full errors in normal operation
- All existing tests pass
