# Design: Early Stop for IVF Cluster Probing

## Architecture

The early stop integrates into the existing `ProbeAndDrainInterleaved` loop with minimal changes:

```
ProbeAndDrainInterleaved:
for (i = 0; i < sorted_clusters.size(); ++i) {
    ┌─────────────────────────────────────────────┐
    │ EARLY STOP CHECK (new)                      │
    │ if collector.Full() &&                      │
    │    collector.TopDistance() < conann.d_k()    │
    │    → record stats, break                    │
    └──────────────────┬──────────────────────────┘
                       │ (not triggered)
                       ▼
    WaitAndPoll → DispatchCompletion (updates TopK)
                       │
                       ▼
    ProbeCluster → Submit I/O
                       │
                       ▼
    Sliding window refill
}
         │
         ▼
    FinalDrain (drains ALL pending I/O — correctness preserved)
```

## Key Design Decisions

### 1. Threshold = d_k (not tau_in)

`d_k` is the raw calibrated distance — the 99th-percentile top-k distance across the dataset. Using `d_k` directly (rather than `tau_in = d_k - 2ε`) is less aggressive: we stop only when TopK quality exceeds the calibration baseline, not when it exceeds the SafeIn boundary.

### 2. Check at loop start (not end)

The check is placed **before** WaitAndPoll for the next cluster, because:
- After the previous iteration's WaitAndPoll + DispatchCompletion, the TopK heap reflects the latest reranked results
- Checking before committing to the next cluster avoids wasted work

### 3. No cancellation of prefetched cluster blocks

The sliding window may have already submitted I/O for 2-3 future cluster blocks. After breaking:
- These completions arrive during `FinalDrain`
- `DispatchCompletion` puts them in `ready_clusters_` but nobody calls `ProbeCluster`
- Memory is properly cleaned up when `ready_clusters_` is destroyed
- The wasted I/O is at most `prefetch_depth` cluster blocks — negligible

### 4. ConANN access

`OverlapScheduler` already holds `index_` which exposes `conann()`. No new dependencies needed.

## Changes

### search_context.h — SearchStats

Add two fields:
```cpp
bool early_stopped = false;
uint32_t clusters_skipped = 0;
```

### overlap_scheduler.cpp — ProbeAndDrainInterleaved

Insert at the top of the for-loop body:
```cpp
// Early stop: TopK quality already exceeds calibration baseline
if (ctx.collector().Full() &&
    ctx.collector().TopDistance() < index_.conann().d_k()) {
    ctx.stats().early_stopped = true;
    ctx.stats().clusters_skipped =
        static_cast<uint32_t>(sorted_clusters.size() - i);
    break;
}
```

## Correctness Argument

- **No results lost**: All I/O submitted before the break is drained by `FinalDrain`. The TopK heap already contains results better than `d_k`, and remaining clusters (further from the query) are statistically unlikely to contain better candidates.
- **Backward compatible**: When `TopDistance() >= d_k` (the common case for hard queries), behavior is identical to before — all `nprobe` clusters are probed.
- **Payload fetch unaffected**: `FetchMissingPayloads` runs after `FinalDrain` and operates on the finalized TopK, regardless of how many clusters were probed.
