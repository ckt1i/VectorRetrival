## Context

当前 warm query 路径已经在每次 search 时预先计算一次 `rotated_q_`，因此剩余的 per-cluster 成本主要集中在 `PrepareQueryRotatedInto`。该函数在 `ClusterProber::Probe` 能执行 Stage 1 FastScan 之前，仍然需要完成一次 subtract-and-norm、一轮 normalize-and-sign-code、一次完整 query 的 14-bit quantization，以及一次单独的 LUT 构建。

当前 warm-serving anchor points 的 profiling 表明，query loop 内部最主要的 CPU 工作不是 rerank 或 payload 处理，而是对每个 probed cluster 重复构建 prepared-query。这里最可疑的部分是当前的两阶段 `quant_query -> BuildFastScanLUT` 管线，因为 `quant_query` 在 query pipeline 中除了生成 LUT 外没有其他消费路径。

这次变更只针对 prepared-query 热路径，不改变 `.clu` loading mode、async submit 策略、CRC policy 或 benchmark operating points。

## Goals / Non-Goals

**Goals:**
- 通过移除 FastScan 路径中的重复遍历和中间 materialization，降低 `PrepareQueryRotatedInto` 的 per-cluster query preparation 成本。
- 保留 Stage 1 FastScan 和 Stage 2 ExRaBitQ 所需的现有 `PreparedQuery` 契约。
- 保持 recall、SafeIn/SafeOut 行为和 benchmark 语义不变。
- 通过单元等价性测试和现有 warm benchmark 点，使优化结果可测量。

**Non-Goals:**
- 修改 cluster probing policy、CRC early stop 逻辑、rerank 行为或 payload I/O。
- 重构 Stage 2 ExRaBitQ 表示形式或移除 `pq.rotated`。
- 引入新的 public API 或改变 benchmark 配置语义。
- 优化 cold-start 行为。

## Decisions

### 1. Fuse FastScan quantization and LUT construction inside the query-prep path

实现上将把 query quantization 和 LUT generation 视为一个统一的 prepared-query 子过程，而不是两个独立阶段。

Rationale:
- `EstimateDistanceFastScan` 消费的是 `lut_aligned`、`fs_width`、`fs_shift` 和 `sum_q`，并不直接消费 `quant_query`。
- 当前路径会先写出完整的 `quant_query` 数组，再在 `BuildFastScanLUT` 中立即读回。
- 去掉这个中间表示是降低 per-cluster CPU 和内存流量的最直接方式。

Alternative considered:
- 保留当前拆分，只对 `QuantizeQuery14Bit` 做微优化。
  这种方式风险更低，但仍然保留了 `quant_query` 的额外写入/读取。

### 2. Preserve `PreparedQuery.rotated` and the downstream Stage 1/Stage 2 interface

这次变更会保留 `PreparedQuery.rotated`、`sum_q`、`norm_qc`、`norm_qc_sq` 以及 Stage 1 的输出字段。

Rationale:
- Stage 2 `IPExRaBitQ` 直接消费 `pq.rotated`。
- `FastScanDequantize` 依赖 `sum_q`、`fs_width` 和 `fs_shift`。
- 保留这个契约可以把改动限制在 prepared-query 生产侧，避免高风险的 probe-path 重写。

Alternative considered:
- 将 prepared-query 构建拆成一个仅供 Stage 1 使用的表示和一个按需生成的 Stage 2 表示。
  这可能减少 Stage 1-only 场景的工作量，但会引入额外结构复杂度，且当前 profiling 没有证据表明它会优于融合 FastScan-prep 的路径。

### 3. Keep the external benchmark and search interfaces unchanged

这次变更将保持在 query preparation 和 SIMD support code 内部。Search configuration、benchmark flags 和 warm-serving operating points 保持不变。

Rationale:
- 当前目标是优化已有热路径，而不是暴露一个新模式。
- 保持外部接口稳定可以让前后对比更直接。

Alternative considered:
- 增加一个 debug flag，在运行时切换旧路径和融合后的 FastScan-prep 路径。
  这有利于 A/B 测试，但也会增加分支和维护成本。更优先的方式是在测试中验证等价性，并直接 benchmark 更新后的实现。

### 4. Validate equivalence at the LUT and distance-estimation boundary

这次变更会在三个层面进行验证：
- 融合后的 LUT 输出与当前 `QuantizeQuery14Bit` + `BuildFastScanLUT` 序列的对比，
- `PrepareQueryRotatedInto` 的 prepared-query 等价性，
- 当前高召回 warm anchor points 上的 query-level benchmark 对比。

Rationale:
- 这样可以把正确性检查限定在优化真正改变行为的位置。
- 这样也避免只依赖 end-to-end recall，因为那可能漏掉微小的 estimator drift。

## Risks / Trade-offs

- [风险] 融合 quantization 和 LUT generation 可能改变底层 rounding 或 shift 行为。-> 缓解：在定向测试中对比 `fs_width`、`fs_shift`、LUT bytes 和 FastScan distance estimates。
- [风险] 去掉 `quant_query` materialization 会让代码更难理解或调试。-> 缓解：保持 helper 边界清晰，并在测试中保留 scalar/reference 路径。
- [风险] 如果 `normalize + sign + sum` 才是主导子步骤，这次优化的收益可能只有边际提升。-> 缓解：在 prepared-query 子步骤层面做前后测量，如果 benchmark 提升不明显就停止。
- [风险] 仅面向 SIMD 的融合可能在 AVX-512、AVX2 和 scalar fallback 之间产生偏差。-> 缓解：保留共享的 scalar reference，并扩展现有 SIMD prepare-query 测试覆盖。
