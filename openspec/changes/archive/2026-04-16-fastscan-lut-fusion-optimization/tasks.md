## 0. 第一版结论固化

- [x] 0.1 保留第一版 prepared-query 重构、测试和 benchmark 记录，作为当前 reference 路径与第二版对照基线。
- [x] 0.2 记录当前结论：第一版实现通过了等价性验证，但在主锚点上未达到性能目标，因此不能作为最终优化提交。
- [x] 0.3 记录当前系统状态：warm query 路径为 CPU-bound，`cpu ~= avg_query`、`io_wait ~= 0`，后续不再围绕 I/O / submit 路径继续扩展这条优化线。

## 1. 第二版热点观测

- [x] 1.1 在 `PrepareQueryInto` / `PrepareQueryRotatedInto` 中加入 prepared-query 子步骤级计时，至少拆分 `subtract_norm`、`normalize_sign_sum`、`quantize`、`lut_build` 四段。
- [x] 1.2 在当前主锚点 `bits=4, epsilon=0.90, nprobe=512, full_preload` 下复测 `alpha=0.01` 和 `alpha=0.02`，确认第二版实现前真正的主热点仍然是 `quantize + LUT`。
- [x] 1.3 如果热点未集中在 `quantize + LUT`，则更新结论并停止这条 change，不继续推进第二版融合实现。（本轮不触发停止；热点主要集中在 `lut_build`）

## 2. 第二版 Prepared-Query Refactor

- [x] 2.1 在 `RaBitQEstimator` 中保留当前 reference 路径，同时为第二版实现预留真正的 fused FastScan-preparation helper。
- [x] 2.2 实现真正的 `quantize + LUT` 数据路径融合，避免“先完整量化到 scratch，再完整读取构建 LUT”的伪融合。
- [x] 2.3 更新 `PrepareQueryInto` 和 `PrepareQueryRotatedInto`，使 query path 可以切换到第二版 fused helper，同时继续保留 `pq.rotated`、`pq.sum_q`、`pq.norm_qc` 和 `pq.norm_qc_sq`。（benchmark 失败后已恢复 reference 为默认）

## 3. 等价性与 ISA 验证

- [x] 3.1 保留并复用第一版 SIMD/reference 测试，继续验证 `fs_width`、`fs_shift` 和 LUT bytes 等价。
- [x] 3.2 保留并复用 query-preparation 测试，继续验证 `PrepareQueryRotatedInto` 的 Stage 1 / Stage 2 语义不变。
- [x] 3.3 扩展或补充测试，覆盖第二版 fused helper 与 reference 路径的 helper-level 等价性。
- [ ] 3.4 验证 AVX-512、AVX2 和 scalar fallback 构建在 requirement 层面保持一致。

## 4. Benchmark Gate

- [x] 4.1 在第二版实现后运行当前 warm-serving anchor points，并将 `probe_ms`、`e2e_ms` 和 recall 与第一版 reference 路径及历史主曲线对比。
- [x] 4.2 验收标准：`alpha=0.01` 与 `alpha=0.02` 两点 recall 不漂移，且 `avg_query` 回到不差于历史 `1.5114 ms / 1.5098 ms` 的 `+5%` 以内。（本轮未通过）
- [x] 4.3 如果第二版仍无法通过验收标准，则将该 change 明确标记为停止，不继续扩展到更广的 probe-path 或 rerank 重写。
