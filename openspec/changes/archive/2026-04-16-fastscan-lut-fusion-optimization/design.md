## Context

当前 warm query 路径已经在每次 search 时预先计算一次 `rotated_q_`，因此剩余的 per-cluster 成本主要集中在 `PrepareQueryRotatedInto`。该函数在 `ClusterProber::Probe` 能执行 Stage 1 FastScan 之前，仍然需要完成一次 subtract-and-norm、一轮 normalize-and-sign-code、一次完整 query 的 14-bit quantization，以及一次单独的 LUT 构建。

当前 warm-serving anchor points 的 profiling 表明，query loop 内部最主要的 CPU 工作不是 rerank 或 payload 处理，而是对每个 probed cluster 重复构建 prepared-query。这里最可疑的部分是当前的两阶段 `quant_query -> BuildFastScanLUT` 管线，因为 `quant_query` 在 query pipeline 中除了生成 LUT 外没有其他消费路径。

第一版实现已经把 query path 对 `quant_query` 的长期持有去掉，并补齐了 LUT / estimator 等价性测试，但 benchmark 没有得到正收益。对主锚点重新测量后，查询阶段表现为 `cpu ~= avg_query`、`io_wait ~= 0`、`CPU utilization ~= 0.996`，说明当前瓶颈是单核 CPU 计算本身，而不是 I/O 等待或调度欠饱和。这个结果要求第二版设计必须更严格地区分“语义融合”和“真正减少指令/内存流量的融合”。

这次变更只针对 prepared-query 热路径，不改变 `.clu` loading mode、async submit 策略、CRC policy 或 benchmark operating points。

## Goals / Non-Goals

**Goals:**
- 通过移除 FastScan 路径中的重复遍历和中间 materialization，降低 `PrepareQueryRotatedInto` 的 per-cluster query preparation 成本。
- 将当前“正确但无收益”的第一版重构收敛为参考路径，并在此基础上推进第二版真正的 `quantize + LUT` 融合。
- 在进入第二版实现前，引入 prepared-query 子步骤级计时，明确热点是否仍集中在 `quantize + LUT`。
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

实现上将把 query quantization 和 LUT generation 视为一个统一的 prepared-query 子过程，而不是两个独立阶段。这里的“融合”特指减少数据写回和再次读取，而不只是将两个 helper 包装到同一个入口下。

Rationale:
- `EstimateDistanceFastScan` 消费的是 `lut_aligned`、`fs_width`、`fs_shift` 和 `sum_q`，并不直接消费 `quant_query`。
- 当前路径会先写出完整的 `quant_query` 数组，再在 `BuildFastScanLUT` 中立即读回。
- 去掉这个中间表示是降低 per-cluster CPU 和内存流量的最直接方式。

Alternative considered:
- 保留第一版当前的 scratch-buffer 路径。
  这种方式已经验证了功能正确，但 benchmark 仍慢于历史主曲线，因此不再被视为可接受终态。

### 2. Instrument prepared-query substeps before finalizing the second-pass implementation

第二版实现前，将在 `PrepareQueryInto` / `PrepareQueryRotatedInto` 内加入局部计时，最少拆分为 `subtract_norm`、`normalize_sign_sum`、`quantize`、`lut_build` 四段。

Rationale:
- 当前系统已经确认是 CPU-bound，但还需要进一步确认 `quantize + LUT` 是否仍是最重子步骤。
- 如果热点已经转移到 `normalize + sign + sum`，则单纯推进 LUT 融合的收益会受限。
- 这组计时结果将决定第二版实现是否应只聚焦 FastScan，还是及时停止这条线。

Alternative considered:
- 跳过局部计时，直接实现第二版 fused helper。
  这样推进更快，但无法解释第二版若再次无收益时究竟是设计方向错误，还是热点判断错误。

### 3. Preserve `PreparedQuery.rotated` and the downstream Stage 1/Stage 2 interface

这次变更会保留 `PreparedQuery.rotated`、`sum_q`、`norm_qc`、`norm_qc_sq` 以及 Stage 1 的输出字段。

Rationale:
- Stage 2 `IPExRaBitQ` 直接消费 `pq.rotated`。
- `FastScanDequantize` 依赖 `sum_q`、`fs_width` 和 `fs_shift`。
- 保留这个契约可以把改动限制在 prepared-query 生产侧，避免高风险的 probe-path 重写。

Alternative considered:
- 将 prepared-query 构建拆成一个仅供 Stage 1 使用的表示和一个按需生成的 Stage 2 表示。
  这可能减少 Stage 1-only 场景的工作量，但会引入额外结构复杂度，且当前 profiling 没有证据表明它会优于融合 FastScan-prep 的路径。

### 4. Keep the external benchmark and search interfaces unchanged

这次变更将保持在 query preparation 和 SIMD support code 内部。Search configuration、benchmark flags 和 warm-serving operating points 保持不变。

Rationale:
- 当前目标是优化已有热路径，而不是暴露一个新模式。
- 保持外部接口稳定可以让前后对比更直接。

Alternative considered:
- 增加一个 debug flag，在运行时切换旧路径和融合后的 FastScan-prep 路径。
  这有利于 A/B 测试，但也会增加分支和维护成本。更优先的方式是在测试中验证等价性，并直接 benchmark 更新后的实现。

### 5. Use benchmark gating to decide whether to keep or stop R068

这次变更会在三个层面进行验证：
- 融合后的 LUT 输出与当前 `QuantizeQuery14Bit` + `BuildFastScanLUT` 序列的对比，
- `PrepareQueryRotatedInto` 的 prepared-query 等价性，
- 当前高召回 warm anchor points 上的 query-level benchmark 对比。

Rationale:
- 这样可以把正确性检查限定在优化真正改变行为的位置。
- 这样也避免只依赖 end-to-end recall，因为那可能漏掉微小的 estimator drift。
- 第一版已经证明“语义等价”本身不足以支持保留实现，因此第二版必须把 benchmark 结果作为保留条件。

Acceptance gate:
- 主锚点固定为 `bits=4, epsilon=0.90, nprobe=512, full_preload`。
- 至少对 `alpha=0.01` 与 `alpha=0.02` 复测。
- recall 不漂移。
- `avg_query` 需要回到不差于历史主曲线 `1.5114 ms / 1.5098 ms` 的 `+5%` 以内；否则这条优化线停止，不继续扩展到 probe-path 重写。

## Risks / Trade-offs

- [风险] 融合 quantization 和 LUT generation 可能改变底层 rounding 或 shift 行为。-> 缓解：在定向测试中对比 `fs_width`、`fs_shift`、LUT bytes 和 FastScan distance estimates。
- [风险] 去掉 `quant_query` materialization 会让代码更难理解或调试。-> 缓解：保持 helper 边界清晰，并在测试中保留 scalar/reference 路径。
- [风险] 如果 `normalize + sign + sum` 才是主导子步骤，这次优化的收益可能只有边际提升。-> 缓解：在 prepared-query 子步骤层面做前后测量，如果 benchmark 提升不明显就停止。
- [风险] 仅面向 SIMD 的融合可能在 AVX-512、AVX2 和 scalar fallback 之间产生偏差。-> 缓解：保留共享的 scalar reference，并扩展现有 SIMD prepare-query 测试覆盖。
- [风险] 第二版实现仍然无法优于当前 reference 路径。-> 缓解：将 benchmark gate 写入任务与验收条件，避免继续在错误方向上扩展复杂度。
