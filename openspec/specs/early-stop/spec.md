# Spec: Early Stop for IVF Cluster Probing

## Overview

Early termination of cluster probing when the TopK result heap is sufficiently populated with high-quality results, as determined by the precomputed ConANN reference distance `d_k`.

## Behavior

### Early Stop Condition

The system checks the following condition **after** each cluster probe and I/O submission:

```
config.early_stop AND collector.Full() AND collector.TopDistance() < conann.d_k()
```

Where:
- `config.early_stop`: User-configurable flag (default `true`)
- `collector.Full()`: The TopK heap contains at least `top_k` results
- `collector.TopDistance()`: The worst (largest) distance in the TopK heap
- `conann.d_k()`: The globally calibrated reference distance from build-time sampling

The TopK heap state reflects completions naturally consumed during `WaitAndPoll` (while waiting for cluster blocks). No explicit drain is performed — the check is opportunistic.

### When Triggered

1. Record `early_stopped = true` and `clusters_skipped = remaining cluster count` in SearchStats
2. Break out of the probe loop
3. Continue to FinalDrain (all pending I/O is consumed normally)
4. Continue to FetchMissingPayloads and AssembleResults as usual

### When NOT Triggered

The probe loop runs to completion (all `nprobe` clusters), identical to pre-early-stop behavior.

## Configuration

- `SearchConfig::early_stop` (bool, default `true`): Enable/disable early stopping. Set to `false` for exhaustive search when exact results are required.

## Statistics

SearchStats gains:
- `early_stopped` (bool): Whether early stopping was triggered for this query
- `clusters_skipped` (uint32_t): Number of clusters skipped (0 if not triggered)

Statistics are available via `SearchResults::stats()`.

## Constraints

- No changes to ConANN classifier logic
- Already-submitted cluster block I/O is NOT cancelled (drained normally)
- No minimum probe count enforced
- With synchronous I/O backends (PreadFallbackReader), early stop may not trigger because vec completions are not consumed during WaitAndPoll; this is expected — the feature targets async I/O (io_uring)
