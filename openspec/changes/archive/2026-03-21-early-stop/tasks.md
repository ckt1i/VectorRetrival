# Tasks: Early Stop for IVF Cluster Probing

## Task 1: Add early stop stats to SearchStats

**File:** `include/vdb/query/search_context.h`

- [x] Add `bool early_stopped = false;` to SearchStats
- [x] Add `uint32_t clusters_skipped = 0;` to SearchStats

## Task 2: Implement early stop check in ProbeAndDrainInterleaved

**File:** `src/query/overlap_scheduler.cpp`

- [x] Insert early stop condition at the top of the for-loop body in `ProbeAndDrainInterleaved`
- [x] Condition: `ctx.collector().Full() && ctx.collector().TopDistance() < index_.conann().d_k()`
- [x] On trigger: set `ctx.stats().early_stopped = true`, compute `clusters_skipped`, break
- [x] Verify FinalDrain correctly handles the early exit (no changes expected)

## Task 3: Unit test

**File:** `tests/query/early_stop_test.cpp`

- [x] Test that early stop triggers when TopK heap has distances below d_k
- [x] Test that early stop does NOT trigger when TopK distances are above d_k
- [x] Test that result quality is preserved (early-stopped results match or are close to full-probe results)
- [x] Add test target to CMakeLists.txt
