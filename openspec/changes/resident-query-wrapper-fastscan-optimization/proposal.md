## Why

当前 `full_preload + use_resident_clusters=1 + nprobe=64` 的主查询路径里，`coarse_select_ms`、`probe_prepare_ms` 和 `probe_stage1_ms` 仍然占了主要时间，其中 `PrepareQueryRotatedInto`、`BuildFastScanLUT`、`EstimateDistanceFastScan` 以及 resident 路径中的候选提交组织成本都还偏重。现有实现已经把 CRC 在线定标和部分通用路径拆开了，下一步需要优先处理真正卡在 query 主路径里的固定开销，并且在实现后立即用同口径 E2E 与 query-only perf 验证收益。

现在推进这项变更，是因为现有 perf 已经足够清楚地指出热点不再集中在 rerank，而是集中在 resident 查询包装、FastScan 准备/分类和 coarse select 等前半段路径。如果不先把这些 P0 路径压薄，后续再做更深层的 SIMD 或算子级优化，收益会被现有厚路径吞掉。

## What Changes

- 为 resident 模式引入更薄的 query wrapper / hot path，把 query 无关或可复用的准备结构进一步前移到 prepare/init 阶段，减少每次 query 的对象组装、访存跳转和包装开销。
- 重构单 bit FastScan 的准备与执行路径，优先压缩 `PrepareQueryRotatedInto`、`BuildFastScanLUT` 和 Stage1 分类主循环中的常数项。
- 为 resident + single assignment 路径增加更轻量的候选提交组织方式，避免保守的全局去重和高频小粒度提交继续放大 `probe_submit_ms`。
- 优化 coarse select 的查询路径，但本轮优先只做两步：先引入可复用的 coarse scratch，去掉 `FindNearestClusters()` 查询期的临时分配与重建；再收紧 coarse score buffer 和 cluster id 映射布局，降低 centroid ranking 的写入、搬运和选择开销。完成这两步后，先做一次 profiling，再决定是否继续进入 top-n selection 的第三步重写。
- 在 benchmark 中保留并扩展现有 query 主路径细分统计，要求实现完成后必须补跑同参数 full E2E 和 query-only perf，并输出可对照的性能与热点结果。

## Capabilities

### New Capabilities
- `resident-query-hotpath`: 定义 resident 常驻聚类模式下的超轻量查询包装与候选提交路径，覆盖 query wrapper 复用、固定容量临时结构、resident 单 assignment 轻量提交策略等行为要求。
- `fastscan-stage1-optimization`: 定义单 bit FastScan 在 query prepare、LUT 构建与 Stage1 分类阶段的优化要求，约束哪些工作需要前移、哪些工作必须保留在线执行。

### Modified Capabilities
- `query-pipeline`: 修改 resident 查询路径要求，允许 query wrapper 复用、coarse select 专项优化、以及更轻量的 resident candidate submit 路径进入正式语义。
- `e2e-benchmark`: 修改 benchmark 验收要求，要求对这轮 P0 优化输出同参数 full E2E 与 query-only perf 结果，并稳定输出细化后的 query 主路径时间字段。

## Impact

- 受影响代码主要包括 resident 查询调度/包装路径、FastScan prepare 与 stage1 执行代码、coarse select 相关实现，以及 `benchmarks/bench_e2e.cpp` 的统计与结果输出逻辑。
- 不引入新的外部服务依赖，也不计划改变已有主命令的基本使用方式，但内部 query 执行结构和 benchmark 验收口径会进一步收敛到 query 主路径。
- 这项变更默认以当前 resident benchmark 参数为主验收路径：`coco_100k`、`nlist=2048`、`nprobe=64`、`full_preload`、`use_resident_clusters=1`、`early_stop=0`、`crc=1`。
