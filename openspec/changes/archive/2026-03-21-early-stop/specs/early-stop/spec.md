# Spec: Early Stop for IVF Cluster Probing

## Overview

Early termination of cluster probing when the TopK result heap is sufficiently populated with high-quality results, as determined by the precomputed ConANN reference distance `d_k`.

## Behavior

### Early Stop Condition

The system SHALL check the following condition at the beginning of each cluster probe iteration:

```
collector.Full() AND collector.TopDistance() < conann.d_k()
```

Where:
- `collector.Full()`: The TopK heap contains at least `top_k` results
- `collector.TopDistance()`: The worst (largest) distance in the TopK heap
- `conann.d_k()`: The globally calibrated reference distance from build-time sampling

### When Triggered

1. Record `early_stopped = true` and `clusters_skipped = remaining cluster count` in SearchStats
2. Break out of the probe loop
3. Continue to FinalDrain (all pending I/O is consumed normally)
4. Continue to FetchMissingPayloads and AssembleResults as usual

### When NOT Triggered

The probe loop runs to completion (all `nprobe` clusters), identical to current behavior.

## Statistics

SearchStats gains:
- `early_stopped` (bool): Whether early stopping was triggered for this query
- `clusters_skipped` (uint32_t): Number of clusters skipped (0 if not triggered)

## Constraints

- No new configuration parameters required
- No changes to ConANN classifier logic
- No changes to SearchConfig
- Already-submitted cluster block I/O is NOT cancelled (drained normally)
- No minimum probe count enforced
