## Why

当前 `coco_100k / nlist=2048 / nprobe=64 / full_preload / use_resident_clusters=1` 的 resident 查询主路径里，coarse score 已经被压到较低水平，新的热点集中在 `PrepareQueryRotatedInto`、`BuildFastScanLUT`、Stage1 的逐 lane `iterate/classify`，以及候选提交路径。继续做零散的算子微调，收益会被现有“胖 query wrapper + 逐候选控制流”吞掉，因此需要先把 query prepare 和 Stage1 的结构改成更适合批量和 SIMD 的形态。

现在推进这项变更，是因为现有 full E2E 与 clean perf 已经把热点边界说明白了：`PrepareQueryRotatedInto` 下游的 LUT 构建仍然偏重，Stage1 中 `mask` 不是问题，真正重的是 `iterate + classify + submit` 这条碎粒度路径。只有先把这两段改造成 batch-friendly 的主路径，后续更深层的 SIMD 化才有明确落点。

## What Changes

- 引入更清晰的 FastScan query wrapper / prepared view 结构，把 resident 查询路径中的 query-global 状态、cluster-local prepared state 和 FastScan-ready 状态拆开，减少每个 cluster probe 时的对象组装与中间态搬运。
- 重构 `PrepareQueryRotatedInto`，把当前 pre-rotated residual 准备路径继续收敛成 query-wrapper 驱动的 prepare 流程，并为后续 fused `quantize + LUT build` 和更深层 SIMD 化预留稳定接口。
- 把 Stage1 从“逐 lane 发现候选、逐 lane classify、逐 lane decode 地址、逐 lane submit”改成“按 block 先 compact 候选，再批量 classify / decode / submit”的 block-driven 路径。
- 为 resident + single-assignment 路径增加 batch candidate submit 能力，包括 block-local candidate buffer、地址批量解码接口和 sink 的 batch 消费接口。
- 在 benchmark 与 profiling 口径中明确区分“用于 E2E 解释的细粒度计时”和“用于 clean perf 的低干扰模式”，并要求在这两项优化实现后重新跑同参数 full E2E 与 query-only perf。
- 在设计中单独列出“第二轮可继续 SIMD 化”的稳定批量路径，包括但不限于：fused quantize/LUT build、block classify mask 生成、地址批量解码、batch submit 前的数据整理等，并要求实现后基于新主路径再做一次热点复盘。
- 将 `PrepareQueryRotatedInto` 的重构目标再细化为：`query-global wrapper` 负责常驻和复用 scratch，`cluster-local` 只产生轻量临时 view，`stage1/stage2` 消费固定 ABI 的 view，避免每 cluster 重建/长期持有 `PreparedQuery` 的大状态。
- 明确 `prepare` 边界清理后的验证口径：`prepare_prepare_ms` 应下降后对应到 `probe_prepare_ms`，并能通过 clean perf 与 full E2E 同步说明 `probe_stage1` 与 `submit` 的收益分离来源。

## Capabilities

### New Capabilities
- `fastscan-query-wrapper`: 定义 resident FastScan 查询路径中的 query wrapper / prepared view 结构、pre-rotated prepare 边界，以及 prepare 阶段与 probe 阶段之间的数据责任划分。
- `stage1-block-batch-classify`: 定义 Stage1 从逐 lane classify 迁移到 block-driven candidate compaction、批量 classify、批量地址解码和批量提交的行为要求。

### Modified Capabilities
- `simd-prepare-query`: 现有 prepare-query SIMD 能力需要扩展到 query-wrapper 形态下的 prepare 语义，并为后续 fused `quantize + LUT build` 和 prepare 子步骤观测保留正式要求。
- `query-pipeline`: resident 查询路径的正式要求需要允许 block-local candidate buffer、batch sink 接口和 address batch decode 进入主查询语义。
- `e2e-benchmark`: benchmark 要求需要覆盖两类口径：保留 full E2E 细粒度统计，并支持用于 clean perf 的低干扰计时模式。

## Impact

- 受影响代码将主要集中在 [rabitq_estimator.h](/home/zcq/VDB/VectorRetrival/include/vdb/rabitq/rabitq_estimator.h)、[rabitq_estimator.cpp](/home/zcq/VDB/VectorRetrival/src/rabitq/rabitq_estimator.cpp)、[cluster_prober.h](/home/zcq/VDB/VectorRetrival/include/vdb/index/cluster_prober.h)、[cluster_prober.cpp](/home/zcq/VDB/VectorRetrival/src/index/cluster_prober.cpp)、[parsed_cluster.h](/home/zcq/VDB/VectorRetrival/include/vdb/query/parsed_cluster.h)、[overlap_scheduler.h](/home/zcq/VDB/VectorRetrival/include/vdb/query/overlap_scheduler.h)、[overlap_scheduler.cpp](/home/zcq/VDB/VectorRetrival/src/query/overlap_scheduler.cpp) 和 [bench_e2e.cpp](/home/zcq/VDB/VectorRetrival/benchmarks/bench_e2e.cpp)。
- 不新增新的外部依赖，也不计划引入新的主 CLI；新的 profiling 区分将继续通过 benchmark 参数控制。
- 默认验收口径仍然以当前 resident 参数为主：`coco_100k`、`nlist=2048`、`nprobe=64`、`full_preload`、`use_resident_clusters=1`、`early_stop=0`、`crc=1`。
- 这项变更会成为后续第二轮 SIMD 优化的基础，因此设计和任务中需要明确列出哪些批量路径在这次重构后会稳定下来，并适合作为下一轮 SIMD 目标。
