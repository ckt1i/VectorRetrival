## Context

当前仓库支持 single assignment 和 redundant top-2 assignment，但 secondary assignment 是在 single-assignment clustering 完成之后再推导出来的，且 centroids 不会因为 overlap membership 的引入而继续更新。最近的 coarse-cover 诊断表明，这种设计在 high-recall 区间是不够的：即使在 pure IVF+Flat probing 语义下，当前 partition 也需要非常大的 probe budget 才能找回大多数 top-10 邻居。

本 change 旨在把现有 overlap 机制从 post-hoc duplication 升级成 recall-aware overlap partitioning。目标不只是提高 probe 后的 candidate recall，而是从 coarse partition 层面降低达到目标 coarse cover 所需的 `nprobe`。

## Goals / Non-Goals

**Goals:**
- 引入 recall-aware secondary assignment 策略，利用 neighbor- 或 query-derived 信号选择互补 overlap cluster。
- 增加 overlap-aware centroid refinement，使 coarse centroids 在多重 assignment 之后继续适应新的 membership。
- 在尽量保持当前存储和 serving 接口不变的前提下，提升 coarse partition 的 high-recall 质量。
- 输出足够的元数据与诊断结果，以对照 residual-aware 与 recall-aware overlap 策略。

**Non-Goals:**
- 不重写 serving-time query pipeline。
- 不要求引入 graph-based serving index，也不试图用图结构替换 IVF。
- 不修改 payload 存储格式或 io_uring 调度行为。
- 不在本 change 中同时完成全套 end-to-end 参数调优；本 change 聚焦 partition 算法本身。

## Decisions

### 1. Secondary assignment 从 residual-aware 升级为 recall-aware
新的 secondary assignment 路径 SHALL 使用 recall-oriented 的互补证据为点选择 second cluster，而不是仅依赖几何 second-nearest 或 residual-style loss。

原因：
- 当前 secondary assignment 虽然增加了 redundancy，但没有显式优化 coarse-cover preservation。
- 对 coarse recall 有价值的 complementary cluster，不一定是几何意义上的 second-best cluster。

备选方案：
- 继续保留 RAIR，只调参数：否决，因为现有诊断表明限制来自结构而不只是参数。
- 只使用 naive top-2 assignment：否决，因为它在 high-recall 区间过弱。

### 2. Recall-aware 证据优先从 local neighbor 结构近似得到
第一版实现 SHOULD 优先使用 local neighbor 结构或 calibration-query 证据来近似 recall-aware signal，而不要求所有数据集都构造完整 query-to-point reverse mapping。

原因：
- 这能在 build-time 提供比 residual loss 更接近 ANN coarse-cover 目标的信号。
- 它避免让第一版实现依赖昂贵、且可能强依赖数据集的 reverse-query indexing。

备选方案：
- 强依赖完整 query-log supervision：否决，因为会让方法难以用于通用数据集和离线构建。
- 仅使用 centroid-distance 启发式：否决，因为不足以解释并修复当前 coarse-cover failure mode。

### 3. Centroid 必须在 overlap membership 下继续 refinement
在 overlap assignment 引入之后，centroids SHALL 基于 overlap membership 重新计算或 refinement，而不是保持 single-assignment clustering 的冻结状态。

原因：
- query routing 由 centroid 几何位置决定。
- 如果 membership 变了而 centroids 不变，那么 partition 在几何上仍然是 single-assignment 的。

备选方案：
- 只复制 postings，不动 centroids：否决，因为这正是当前结构性缺陷。
- 每轮都从头训练复杂的 overlap objective：暂缓，作为可能的后续增强，因为实现复杂度更高。

### 4. 保持现有存储和 serving contract
Build-time 算法可以增加 assignment policy 和 refinement stage 相关元数据，但 SHALL 尽量保持现有 `.clu`、`.data` 和 serving interface 不变，除非后续证据表明确需更大改动。

原因：
- 当前目标是提升 coarse partition，而不是同时引入格式 churn。
- 保持格式兼容有利于旧 partition 与新 partition 的直接对照。

备选方案：
- 同时改动 cluster-storage layout：否决，因为会把 partition 质量与存储变更混在一起，削弱证据解释力。

## Risks / Trade-offs

- **[Risk] Recall-aware assignment 可能改善 coarse cover，但显著增加 build cost** → Mitigation: 第一版优先使用 lightweight 的 neighbor-aware 近似，并复用 builder diagnostics 来量化 trade-off。
- **[Risk] Overlap-aware centroid refinement 可能破坏 cluster balance** → Mitigation: 记录 cluster-size 统计，并与当前 builder 输出对照。
- **[Risk] overlap refinement 可能改善 coarse cover，但损害压缩质量** → Mitigation: 将 coarse-cover 改善与压缩 / serving 指标分离评估，不提前绑定结论。
- **[Risk] query-aware signal 如果设计过窄，可能只对单一数据集有效** → Mitigation: 优先使用 local-neighbor 或 calibration-based 信号，并显式记录 builder identity 和 assignment policy。

## Migration Plan

1. 在当前 `single`、`redundant_top2_naive`、`redundant_top2_rair` 之外，引入新的 recall-aware secondary assignment family。
2. 在显式配置开关之后增加 overlap-aware centroid refinement stage。
3. 导出 assignment policy 和 refinement stage 元数据。
4. 使用 `coarse-builder-and-cover-diagnostics` 提供的 coarse-cover 诊断来验证 probe budget 是否下降。
5. 只有在 coarse-cover 改善被确认之后，才推进新的 serving 对比。

回滚策略：
- 保留现有 assignment policy 作为稳定回退基线。
- 将 recall-aware overlap refinement 完全置于显式配置开关之后，可在不影响当前索引的前提下关闭。

## Open Questions

- 第一版 recall-aware signal 最适合使用什么：local neighbor 分布、calibration-query evidence，还是两者混合？
- overlap-aware centroid refinement 需要多少轮迭代，才能获得显著 coarse-cover 改善？
- primary membership 和 secondary membership 在 refinement 时是否应该等权，还是使用不同权重？
