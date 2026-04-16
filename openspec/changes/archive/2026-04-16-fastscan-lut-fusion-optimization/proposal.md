## Why

当前 warm-serving 路径已经去除了大部分 `.clu` 侧 I/O 开销，剩余差距主要集中在 CPU 侧的查询预处理。现有 profile 显示 `PrepareQueryRotatedInto`、`QuantizeQuery14Bit` 和 `BuildFastScanLUT` 占据了每个 cluster 的热路径，因此下一步优化应当针对 prepared-query 构建，而不是 submit、preload 或 rerank 逻辑。

第一版 `fastscan-lut-fusion-optimization` 已经完成了语义等价的 prepared-query 重构，并通过了定向测试，但 benchmark 结果没有达到预期：在当前主锚点下，修改后的路径仍然慢于历史最佳曲线。进一步测得当前查询阶段 `cpu ~= avg_query`、`io_wait ~= 0`、`CPU utilization ~= 1 core`，说明问题不是 I/O 或 submit，而是单核 CPU 热路径本身。

因此，这份变更文档需要从“完成一次融合重构”调整为“继续推进第二版 R068”：在保持外部契约不变的前提下，落实真正的 `quantize + LUT` 数据路径融合，并以 benchmark 结果决定是否保留这条优化线。

## What Changes

- 保留第一版已经完成的 prepared-query 等价性重构与测试，用作当前 query-prep 路径的参考实现。
- 在 `PrepareQueryInto` / `PrepareQueryRotatedInto` 内增加更细粒度的局部计时，拆分 `subtract_norm`、`normalize_sign_sum`、`quantize`、`lut_build` 四段，确认真正的 CPU 热点。
- 将 FastScan 的 query quantization 和 LUT 构建推进为真正的数据路径融合，避免“先完整量化到 scratch，再完整读取构建 LUT”的伪融合实现。
- 保留现有 `PreparedQuery` 契约，确保 Stage 1 FastScan 和 Stage 2 ExRaBitQ 的 recall 与分类语义不变。
- 增加针对第二版融合路径与当前“两阶段 quantize-then-LUT 路径”等价性的定向验证，并将 benchmark 验收明确化：如果在当前高召回 operating point 上无法回到历史主曲线附近，则停止这条优化线。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `simd-prepare-query`：扩展 SIMD 查询预处理要求，使 FastScan 预处理路径在保持功能等价的前提下，先支持子步骤级观测，再支持真正的 `quantize + LUT` 融合实现。

## Impact

- 受影响代码：`src/rabitq/rabitq_estimator.cpp`、`src/simd/fastscan.cpp`、对应头文件以及 SIMD/query 测试。
- 受影响行为：warm query 路径上每个 cluster 的 prepared-query 构建。
- 外部 API：预期无变化；改动应保持在 query preparation 内部。
- 验证方式：单元等价性测试 + prepared-query 子步骤计时 + 当前 warm-serving anchor points 的前后 benchmark 对比。
