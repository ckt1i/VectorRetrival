## 1. Prepare Hot Path Baseline

- [x] 1.1 Reconfirm the current resident benchmark baseline with `--fine-grained-timing 1` and capture `prepare_subtract`, `prepare_normalize`, `prepare_quantize`, `prepare_lut_build`, and `stage1_estimate`
- [x] 1.2 Audit `PreparedQuery` and `ClusterPreparedScratch` steady-state lifecycle in the resident hot path and identify repeated `resize`, clear, or alignment work that still executes per cluster

## 2. Scratch Reuse and Prepare State Slimming

- [x] 2.1 Refactor resident prepare scratch so steady-state queries reuse fixed-capacity `rotated`, `sign_code`, and FastScan LUT storage without repeated hot-path `resize`
- [x] 2.2 Move `lut_aligned` and equivalent derived scratch state out of the per-cluster hot path whenever the underlying buffer has not changed
- [x] 2.3 Remove redundant prepare-buffer clearing on paths that can guarantee full overwrite, keeping only the minimum tail masking or cleanup needed for correctness

## 3. Fused FastScan Prepare Rework

- [x] 3.1 Rework `QuantizeQuery14BitWithMaxToFastScanLUT()` so quantized group outputs flow directly into LUT generation with less intermediate chunk materialization
- [x] 3.2 Refactor fused prepare so chunk width and equivalent internal strategies remain tunable implementation details rather than fixed dimension assumptions
- [x] 3.3 Preserve reference-equivalence checks so generic and specialized fused prepare paths still match `fs_width`, `fs_shift`, and packed LUT bytes

## 4. Cross-Dimension Validation and Optional Specialization

- [x] 4.1 Validate the optimized generic prepare path on representative dimensions, including non-`2^n` dimensions, before considering any specialization
- [x] 4.2 Only if profiling supports it, evaluate optional specialization as an add-on layer with an explicit fallback to the generic path
- [x] 4.3 Document whether any specialization is worth keeping after cross-dimension measurements

## 5. Stage1 and Observability Follow-up

- [x] 5.1 Keep `prepare_subtract`, `prepare_normalize`, `prepare_quantize`, and `prepare_lut_build` observable after the prepare refactor
- [x] 5.2 Confirm `EstimateDistanceFastScan` remains a distinct optimization boundary and that Stage1 post-estimate control flow is not re-merged into an opaque total
- [x] 5.3 If needed, add Stage2 collect/kernel/scatter timing hooks for follow-up diagnosis without changing result semantics

## 6. Validation

- [x] 6.1 Rebuild and run correctness-oriented checks for the optimized FastScan prepare path, including fused/reference equivalence validation
- [x] 6.2 Re-run the resident benchmark at the target work point with low-overhead timing and verify `probe_prepare_ms` decreases without recall regression
- [x] 6.3 Re-run the same benchmark with `--fine-grained-timing 1` and verify the expected reduction lands primarily in `prepare_quantize` and `prepare_lut_build`
- [x] 6.4 Run the optimized generic path on additional datasets or dimensions and confirm the change does not rely on `dim=512`-specific assumptions
