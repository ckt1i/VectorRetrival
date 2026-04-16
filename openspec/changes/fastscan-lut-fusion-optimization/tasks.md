## 1. Prepared-Query Refactor

- [ ] 1.1 在 `RaBitQEstimator` 中隔离当前 FastScan query-prep 序列，确保 quantization 和 LUT generation 可以独立调整，而不改动 Stage 1/Stage 2 的调用点。
- [ ] 1.2 实现一个融合的 FastScan-preparation 路径，直接从归一化后的旋转 query 推导 `fs_width`、`fs_shift` 和 packed LUT，并且不需要在 query path 中持久化完整的 `quant_query` buffer。
- [ ] 1.3 更新 `PrepareQueryInto` 和 `PrepareQueryRotatedInto`，使其使用融合后的 FastScan-preparation 路径，同时保留 `pq.rotated`、`pq.sum_q`、`pq.norm_qc` 和 `pq.norm_qc_sq`。

## 2. Equivalence Validation

- [ ] 2.1 增加或扩展 SIMD/reference 测试，验证融合后的 FastScan preparation 在 `fs_width`、`fs_shift` 和 LUT bytes 上与现有 `QuantizeQuery14Bit` + `BuildFastScanLUT` 输出一致。
- [ ] 2.2 增加 query-preparation 测试，验证 `PrepareQueryRotatedInto` 仍然产生等价的 Stage 1 FastScan distance estimates，并保留 Stage 2 所需输入。
- [ ] 2.3 验证 AVX-512、AVX2 和 scalar fallback 构建在 requirement 层面保持一致。

## 3. Benchmark Confirmation

- [ ] 3.1 在重构后运行当前 warm-serving anchor points，并将 `probe_ms`、`e2e_ms` 和 recall 与变更前 baseline 对比。
- [ ] 3.2 记录融合路径是否带来显著的 CPU 侧收益；如果没有，则明确记录 R068 应当停止，而不是继续扩展到更广的 probe-path 重写。
