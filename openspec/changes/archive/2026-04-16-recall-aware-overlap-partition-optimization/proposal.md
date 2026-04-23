## Why

在当前仓库的 coarse partition 上，即使启用了 redundant top-2 assignment，在 `coco_100k` 上做 pure IVF+Flat probing 时，若想达到 99% 的 top-10 coarse cover，仍然需要探测 300 个以上的聚类。这说明现有 overlap 机制并没有充分保留 true-neighbor 的 coarse partition 结构。

因此，下一步算法方向应当从 post-hoc residual-aware duplication 升级为 recall-aware overlap partitioning。实际含义是：secondary assignment 应该利用 query- 或 neighbor-derived 的 recall signal 来选择互补簇，同时 centroids 也应在 overlap membership 下继续 refinement，而不是停留在 single-assignment 训练阶段。

## What Changes

- 增加 recall-aware secondary assignment 策略，使用 recall-oriented complementary signal 来选择 overlap cluster，而不是仅依赖 residual-style 或纯几何 second-best 启发式。
- 增加 overlap-aware centroid refinement，使 centroids 在引入 overlap assignment 后继续更新，而不是冻结在 single-assignment clustering 结果上。
- 增加显式元数据与实验输出，用于区分 primary assignment、secondary assignment policy 与 refinement stage。
- 增加评估钩子，用于测量新 overlap partition 是否显著降低达到目标 coarse-cover 所需的 probe budget。
- 尽量保持现有存储格式和 serving 接口稳定，除非确有必要增加元数据。

## Capabilities

### New Capabilities
- `recall-aware-secondary-assignment`: 使用 recall-oriented 的互补选择来分配 secondary cluster，而不是仅依赖 residual-aware 或 distance-only 的 second choice。
- `overlap-aware-centroid-refinement`: 在 overlap membership 下重新计算和 refinement centroids，使 coarse partition 几何结构能适应多重 assignment。

### Modified Capabilities

## Impact

- 影响代码：`IvfBuilder`、clustering 配置、overlap assignment 逻辑、builder 元数据、离线评估脚本，以及对 overlap variant 做对照的实验 summary。
- 影响系统：coarse partition 训练、redundant assignment 策略、诊断实验规划，以及用于选择下一轮 serving 实验的 benchmark 输出。
- 依赖关系：可以复用 `coarse-builder-and-cover-diagnostics` 提供的 builder 诊断链路来验证新 partition 方法的效果。
