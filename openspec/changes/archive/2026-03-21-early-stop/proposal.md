# Proposal: Early Stop for IVF Cluster Probing

## Summary

Add an early stopping mechanism to the IVF query pipeline. When the TopK heap is full and its worst distance falls below the precomputed global reference distance `d_k`, skip remaining cluster probes and proceed directly to the rerank/drain phase.

## Motivation

Currently `ProbeAndDrainInterleaved` always probes exactly `nprobe` clusters regardless of how good the current TopK results are. For queries where the nearest clusters already yield excellent results, probing additional clusters wastes I/O bandwidth and CPU cycles with negligible recall improvement.

The ConANN calibration already computes `d_k` — the 99th-percentile top-k distance across sampled pseudo-queries. If a query's TopK worst distance is below `d_k`, the current result quality already exceeds 99% of calibration queries, making further probing extremely unlikely to improve results.

## Approach

- **Threshold**: Use the existing `conann.d_k()` — no new parameters needed
- **Check point**: Beginning of each iteration in `ProbeAndDrainInterleaved`, after previous cluster's I/O completions have been drained (TopK heap is up-to-date)
- **Condition**: `collector.Full() && collector.TopDistance() < conann.d_k()`
- **Action**: Break the loop, fall through to `FinalDrain` (which drains all in-flight I/O correctly)
- **Stats**: Track `early_stopped` (bool) and `clusters_skipped` (count) in `SearchStats`

## Scope

- Modify `ProbeAndDrainInterleaved` in `overlap_scheduler.cpp` (~5 lines)
- Add 2 fields to `SearchStats` in `search_context.h`
- Unit test verifying early stop triggers and result quality is preserved

## Non-goals

- Dynamic threshold adaptation per-query
- Cancellation of already-submitted cluster block I/O
- Minimum probe count guarantee
