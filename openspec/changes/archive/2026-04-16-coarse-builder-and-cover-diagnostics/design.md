## Context

当前仓库的主要 IVF build 路径由 `IvfBuilder` 提供，自动聚类依赖 `SuperKMeans` 或 `HierarchicalSuperKMeans`，并可选开启 redundant top-2 assignment。过去的性能工作主要集中在 serving 优化和 end-to-end trade-off 上，但最近的 pure IVF+Flat probing 结果表明，当前 coarse partition 本身可能就是 high-recall 区间 recall gap 的主要来源。

与此同时，项目中已经有一个 `IVF+RQ+rerank` baseline，它使用 Faiss 训练 coarse quantizer，并在相近 probe budget 下表现出明显更好的 top-10 candidate coverage。现有代码库缺乏一条 builder 无关的诊断链路，无法回答 coarse cover 的差距究竟主要来自 coarse builder，还是来自当前 overlap assignment 策略，或者二者兼有。

本 change 将围绕 coarse building 与 pure coarse-cover 评估增加一层专用诊断能力。它是明确的离线诊断 change：不会修改 serving 逻辑、CRC 行为、epsilon pruning 或 payload handling。

## Goals / Non-Goals

**Goals:**
- 允许在共享评估契约下，用多个 coarse builder 构建兼容的 IVF coarse partition。
- 输出 pure coarse-cover 诊断结果，将 coarse partition 质量与 query-time pruning 或 early-stop 策略隔离开。
- 在 builder 和 assignment mode 两个维度上导出可直接对照的结果，以便后续分别分析 builder 质量与 overlap 质量。
- 增加结构化实验产物，输出 cover-vs-probe 曲线以及达到目标 cover 所需的 probe 阈值。

**Non-Goals:**
- 不修改现有 serving-time query path、CRC 行为、epsilon 配置或 io_uring 调度。
- 不在本 change 中引入 recall-aware secondary assignment。
- 不修改 `.clu` 或 `.data` 存储格式。
- 不重定义当前 end-to-end benchmark contract。

## Decisions

### 1. 引入一等 coarse builder selector
主 IVF build pipeline SHALL 暴露 coarse builder identity，并在元数据和输出中记录。第一批支持的 builder SHALL 包含：
- `hierarchical_superkmeans`，对应当前仓库默认路径
- `faiss_kmeans`，用于 coarse builder 对照

原因：
- 这是判断 coarse cover 差距是否主要来自 builder 的最小必要改动。
- 它避免后续算法 change 与特定 builder 实现细节耦合。

备选方案：
- 直接用 Faiss 替换当前 builder：否决，因为会失去对照对象。
- 一次性接入很多 builder：否决，因为在第一轮诊断证据形成前，集成复杂度过高。

### 2. 将 pure coarse cover 作为独立评估平面
本 change SHALL 定义 pure coarse-cover 评估模式，测量在 IVF+Flat 语义下，GT top-k 邻居是否落入已探测的 coarse cluster 集合。

原因：
- 当前 end-to-end 指标会把 coarse partition 质量与 cluster 内 pruning、量化、serving 策略混在一起。
- pure coarse cover 能直接回答本 change 的核心问题：为了把 GT top-k 包含进来，究竟需要探测多少 coarse clusters？

备选方案：
- 仅使用现有 vector-search run 的 candidate recall：否决，因为它依然依赖 cluster 内 candidate generation。
- 仅使用端到端 recall：否决，因为距离 coarse partition 太远，不适合做定位。

### 3. assignment-mode 对照与 builder 对照使用同一诊断工作流
诊断工作流 SHALL 在同一输出契约下比较 `single`、`redundant_top2_naive`、`redundant_top2_rair`。

原因：
- 当前问题明确要求区分 coarse builder 差距和 overlap 策略差距。
- 把 assignment 对照纳入同一工作流，可避免输出格式漂移，结果也更容易解释。

备选方案：
- 将 overlap 诊断延后到后续 change：否决，因为 builder-vs-overlap 的歧义将继续存在。

### 4. 同时导出原始曲线和派生阈值
诊断输出 SHALL 同时包含原始 cover-vs-probe 曲线，以及如下派生阈值：
- 达到 `cover >= 0.95` 所需的最小 `nprobe`
- 达到 `cover >= 0.99` 所需的最小 `nprobe`
- 达到 `cover >= 0.995` 所需的最小 `nprobe`

原因：
- 用户当前最关心的是“达到 99% top-10 coarse cover 需要多少个 probe”。
- 派生阈值可以让 builder 和 assignment mode 的差异直接落在可解释的 operating point 上。

备选方案：
- 只导出原始 CSV，阈值由外部脚本计算：否决，因为这样会降低可复现性，也不利于后续实验计划引用。

### 5. 尽量复用现有 benchmark 与输出体系
新的诊断路径 SHOULD 尽量复用现有 benchmark infrastructure、输出目录与 summary 生成习惯，而不是新造一套独立实验框架。

原因：
- 这样能降低维护成本，也便于将新证据接入现有 refine logs 和 formal-study 输出。
- 后续比较脚本会更简单，实验轨迹更统一。

备选方案：
- 在 `scripts/` 下单独建立一套诊断框架：否决，因为会重复已有 manifest 与输出规范。

## Risks / Trade-offs

- **[Risk] 如果不同 builder 的数据准备或 metric 处理存在细微差异，builder 对照仍然可能有歧义** → Mitigation: 强制所有 builder 共享同一 normalization、同一 query set 和同一 coarse-cover 评估契约。
- **[Risk] 仓库最终支持的 builder 种类过多，维护成本上升** → Mitigation: 初始范围只支持两个 builder，并要求所有结果写出明确的 builder identity。
- **[Risk] coarse-cover 改善不一定直接转化为 end-to-end 改善** → Mitigation: 明确本 change 是诊断 change，不提前外推 serving 结论。
- **[Risk] assignment-mode 的诊断结果可能被误读为最终 serving 建议** → Mitigation: 在输出命名中强调其诊断性质，并在 summary 中明确 builder / assignment 标识。

## Migration Plan

1. 在离线 build 配置中加入 builder identity，并写入输出元数据。
2. 增加 pure coarse-cover 诊断 benchmark 路径与结构化输出。
3. 在同一诊断契约下增加 assignment-mode 对照运行。
4. 扩展 benchmark summary / aggregation 流程，使 coarse-cover 结果可直接聚合。
5. 在获得 builder / overlap 差距的证据后，再选择下一步算法 change。

回滚策略：
- 保留现有 builder 路径作为默认路径。
- 如果 Faiss 诊断路径带来问题，可通过配置关闭，而不影响当前 serving path 或 index format。

## Open Questions

- 第一版诊断实现应该是独立 benchmark binary，还是在现有 benchmark target 中增加 mode？
- 阈值 summary 应该在 run 时直接生成，还是在后处理聚合阶段生成？
- 第一版诊断结果应该存放在 formal-study 输出树下，还是单独放到 refine 专用输出目录中？
