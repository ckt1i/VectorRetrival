## Why

当前 warm-serving 路径已经去除了大部分 `.clu` 侧 I/O 开销，剩余差距主要集中在 CPU 侧的查询预处理。现有 profile 显示 `PrepareQueryRotatedInto`、`QuantizeQuery14Bit` 和 `BuildFastScanLUT` 占据了每个 cluster 的热路径，因此下一步优化应当针对 prepared-query 构建，而不是 submit、preload 或 rerank 逻辑。

## What Changes

- 将 FastScan 的 query quantization 和 LUT 构建融合为一个统一的 prepared-query 路径，减少对归一化后的旋转 query 的重复遍历。
- 取消 query-time FastScan 预处理必须先 materialize 完整 `quant_query` buffer 再构建 LUT 的要求。
- 保留现有 `PreparedQuery` 契约，确保 Stage 1 FastScan 和 Stage 2 ExRaBitQ 的 recall 与分类语义不变。
- 增加针对融合路径与当前“两阶段 quantize-then-LUT 路径”等价性的定向验证。
- 增加 benchmark 侧的说明，用于衡量融合路径是否能在当前高召回 operating point 上降低 `probe_ms` 和 `e2e_ms`。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `simd-prepare-query`：扩展 SIMD 查询预处理要求，使 FastScan 预处理路径能够融合 quantization 与 LUT 构建，同时仍与现有实现保持功能等价。

## Impact

- 受影响代码：`src/rabitq/rabitq_estimator.cpp`、`src/simd/fastscan.cpp`、对应头文件以及 SIMD/query 测试。
- 受影响行为：warm query 路径上每个 cluster 的 prepared-query 构建。
- 外部 API：预期无变化；改动应保持在 query preparation 内部。
- 验证方式：单元等价性测试 + 当前 warm-serving anchor points 的前后 benchmark 对比。
