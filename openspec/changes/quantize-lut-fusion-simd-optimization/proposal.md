## Why

现有 query 主路径在完成 query-wrapper、block-driven classify 和 prepare 子阶段观测后，新的热点已经收敛到 `prepare_lut_build` 与 Stage1 survivor 后处理。当前 `prepare` 分解结果表明，`lut_build` 已经成为 prepare 内部的绝对主项，而 Stage1 的 `iterate/classify` 仍然明显重于 `mask` 与 `estimate`。如果继续只做零散微调，收益会被 `quantize -> materialize int16 -> BuildFastScanLUT -> lane-wise classify` 这条偏搬运、偏碎控制流的路径吃掉，因此需要单独开一轮 change，把 `quantize + lut_build` 融合优化与下一轮 SIMD 落点一次性设计清楚。

## What Changes

- 新增一份专门面向 `quantize + lut_build` 融合优化的 change，明确其目标不是单独替换一个 kernel，而是把 prepare 主路径改造成更短的数据通路。
- 设计 `quantize + lut_build` 的融合方案，要求减少 `quant_query` 中间态的完整 materialize / reload，并尽量直接按 FastScan 最终 LUT 布局写出。
- 给出 `BuildFastScanLUT` 在“保留两段”和“完全融合”两种路线下的实现清单、边界条件与推荐优先顺序，并明确当前具体可做的 SIMD 点：
  - `BuildLut16` 的 16-entry group LUT 生成
  - lo/hi byte plane 拆分
  - packed lane layout 直写
  - `QuantizeQuery14BitWithMax` 的 pack/store 压缩
- 明确 Stage1 内部剩余 SIMD 候选清单，包括 survivor compaction、batch classify、`EstimateDistanceFastScan` 后续压缩、`DecodeAddressBatch`、submit 前整理等，并给出优先级。
- 固定后续实现时必须沿用的验收口径：full E2E、query-only perf，以及 prepare/stage1 的细粒度字段。
- 要求后续实现保留现有 recall、排序、CRC、payload 和 resident serving 语义，不把 kernel 优化与语义改动混在一起。

## Capabilities

### New Capabilities
- `fastscan-lut-fusion`: 定义 `quantize + lut_build` 融合路径、最终 LUT 写出契约，以及与现有 FastScan 读取布局的等价关系。

### Modified Capabilities
- `simd-prepare-query`: 现有 prepare-query 要求需要扩展到 fused quantize/LUT 路径，并允许记录“保留两段版”和“融合版”的等价输出与观测口径。
- `query-pipeline`: query 主路径要求需要补充 Stage1 后续 SIMD 候选的正式优化边界，尤其是 survivor compaction、batch classify 与 batch address decode。
- `e2e-benchmark`: benchmark 要求需要继续输出 prepare 与 Stage1 子阶段字段，并支持后续 fused kernel 的同口径比较。

## Impact

- 主要影响 [src/simd/fastscan.cpp](/home/zcq/VDB/VectorRetrival/src/simd/fastscan.cpp)、[include/vdb/simd/fastscan.h](/home/zcq/VDB/VectorRetrival/include/vdb/simd/fastscan.h)、[src/simd/prepare_query.cpp](/home/zcq/VDB/VectorRetrival/src/simd/prepare_query.cpp)、[include/vdb/simd/prepare_query.h](/home/zcq/VDB/VectorRetrival/include/vdb/simd/prepare_query.h) 以及与 prepare/stage1 计时相关的 query / benchmark 路径。
- 不引入新的外部依赖，不计划改变索引格式、FastScan block 格式或 CLI 行为。
- 本 change 的输出是后续实现的详细蓝图，因此设计和任务会比普通 proposal 更强调“实现决策完整性”和 SIMD 清单的可执行程度。
- 当前预计会重点覆盖的 SIMD 落点包括 [src/simd/fastscan.cpp](/home/zcq/VDB/VectorRetrival/src/simd/fastscan.cpp) 中的 LUT 构建与写出路径、[src/simd/prepare_query.cpp](/home/zcq/VDB/VectorRetrival/src/simd/prepare_query.cpp) 中的 reduce/pack 路径，以及 [src/index/cluster_prober.cpp](/home/zcq/VDB/VectorRetrival/src/index/cluster_prober.cpp) 中的 survivor 后处理路径。
