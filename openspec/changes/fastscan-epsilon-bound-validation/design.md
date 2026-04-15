## 背景

当前对 `coco_100k` 的 warm-serving profiling 表明，在 `.clu` `full_preload` 之后，查询 CPU 时间已经主要集中在 `probe` 上，而单个最重的热点是 Stage 2 `IPExRaBitQ`。不过从候选流向看，Stage 2 的开销更像是一个结果，而不是最初原因：大量 Stage 1 中未被判定为 `SafeOut` 的候选会继续流向 Stage 2，并最终在 Stage 2 再次被拒掉。

这里真正不确定的是 `epsilon-percentile` 在系统统一到 FastScan 之后到底代表什么。历史设计里，基于 popcount 的 epsilon 标定和基于 FastScan 的标定曾经分叉过，因此必须先把当前代码路径验证清楚，再决定要不要继续做 CPU 优化：

- `epsilon-percentile` 是否仍然在标定 FastScan `eps_ip`
- 运行时查询行为是否由 `segment.meta.conann.epsilon` 驱动
- 改变 `epsilon-percentile` 是否真的需要重建索引，才能影响 FastScan Stage 1 的边界

这是一个跨模块的实验变更，因为它同时涉及：

- 索引构建阶段的校准语义
- `segment.meta` 在打开索引时的解释方式
- 查询路径中的 FastScan Stage 1 / Stage 2 统计
- benchmark 输出和分析流程

## 目标 / 非目标

**目标：**
- 端到端验证 FastScan epsilon 标定链路，从 CLI / 构建配置到运行时 Stage 1 分类全部确认。
- 定义一个受控的重建-复测流程，只改变 `epsilon-percentile`，其余服务侧参数全部固定。
- 输出足够的 benchmark 结果，用来判断下一步主攻方向是：
  - 更小 / 更紧的 FastScan `eps_ip`
  - 还是直接优化 Stage 2 / SIMD CPU kernel
- 建立明确的决策门槛，避免把构建阶段的 epsilon 调优和无关的 CPU 路径修改混在一起。

**非目标：**
- 本次变更不做查询路径实现修改。
- 不新增 SIMD kernel，也不改 Stage 2 算法。
- 不扩展到超出 epsilon 研究所需的 baseline。
- 不恢复冷启动实验流程。

## 决策

### 决策 1：将 `epsilon-percentile` 视为构建阶段的验证变量，而不是查询阶段的参数

实验设计中会显式区分：

- **构建阶段输入**：`epsilon-percentile`、`epsilon-samples`
- **运行时服务输入**：`nprobe`、`crc-alpha`、`clu_mode`、队列深度、提交模式

原因：
- 当前代码会在 `IvfIndex::Open()` 时从 `segment.meta` 里加载 `conann.epsilon`。
- 当传入 `--index-dir` 时，`bench_e2e` 会跳过构建，因此无法刷新 `eps_ip`。
- 如果不强制重建，epsilon 实验会在同一个运行时边界上重复测试，看起来像做了参数扫描，实际没有变化。

备选方案：
- 把 `epsilon-percentile` 当作运行时扫描参数。
  不采用，因为对预构建索引不会改变运行时行为。

### 决策 2：第一轮 epsilon 实验使用一个固定的服务点

主实验锚定在当前最相关的高召回 warm 点：

- 数据集：`coco_100k`
- `topk=10`
- `queries=1000`
- `nlist=2048`
- `nprobe=200`
- `crc-alpha=0.02`
- `clu_mode=full_preload`

原因：
- 这个区域正是 R067 暴露出 Stage 2 压力过大的地方。
- 保持运行时参数不变，可以把变化隔离到 epsilon 标定本身。
- 使用 `full_preload` 可以避免把 cluster 侧的提交 / 解析开销重新混进来。

备选方案：
- 把 `nprobe` 和 `crc-alpha` 与 `epsilon-percentile` 一起扫。
  第一轮不采用，因为会把候选流变化混淆在一起。

### 决策 3：核心输出必须描述候选流，而不是只看延迟

benchmark 和分析必须输出：

- 加载后的 `eps_ip`
- Stage 1 `SafeOut`
- Stage 1 未 `SafeOut` 或 `Uncertain` 流量
- Stage 2 `SafeOut`
- Stage 2 `Uncertain`
- `probe_ms`
- `rerank_cpu_ms`
- recall 和 p99

原因：
- 我们需要判断的是 Stage 2 过重到底是不是因为 Stage 1 边界太宽。
- 仅看延迟无法区分“kernel 慢”和“太多候选进了 kernel”。

备选方案：
- 只看 `avg_query_ms` 和 `recall@10`。
  不采用，因为无法解释候选流在哪一步发生了变化。

### 决策 4：每个 `epsilon-percentile` 都必须重新构建索引

对于研究中的每个 `epsilon-percentile` 点，都要求：

1. 将索引重建到独立目录
2. 验证 `segment.meta` / 加载后的 `eps_ip`
3. 在同一个 warm-serving 配置下跑 benchmark

原因：
- 这是唯一能保证运行时 FastScan 边界真的变了的方法。
- 也能检查构建输出和加载语义是否一致。

备选方案：
- 只改 `segment.meta`。
  不采用，因为这无法验证完整构建路径，而且太容易误用。

### 决策 5：在开始 CPU kernel 工作之前先设定决策门槛

实验结论按下面的门槛解释：

- 如果降低 `epsilon-percentile` 能明显减少 Stage 2 `SafeOut` 压力，同时召回基本稳定，就优先继续 epsilon 调优并重跑刷新后的 Pareto 曲线。
- 如果降低 `epsilon-percentile` 很快损害召回，或者对 Stage 2 压力几乎没有影响，就停止 epsilon 调优，转去做 Stage 2 / SIMD CPU 优化。

原因：
- 这样可以避免把优化资源投到错误层级。
- 也把当前的 profiling 模糊区转换成一个可执行的实验决策。

## 风险 / 权衡

- [风险] 每个 epsilon 点都要重建索引，会增加实验周转时间。
  → 缓解方式：第一轮只保留一个小网格，比如 `{0.90, 0.95, 0.99}`，并且只跑一个固定服务点。

- [风险] 现有跟踪文档可能仍然把 `epsilon-percentile` 理解为查询阶段参数。
  → 缓解方式：必须显式记录加载后的 `eps_ip`，以及这次运行是使用重建索引还是复用索引。

- [风险] Stage 1 / Stage 2 统计可能仍不足以清楚区分 `SafeIn` 和 `Uncertain`。
  → 缓解方式：benchmark 输出必须包含足够的 Stage 1 和 Stage 2 计数，而不只是最终延迟。

- [风险] 复用 `--index-dir` 时，当前索引元数据可能不会完整反映 CLI 构建参数。
  → 缓解方式：把运行时加载到的 `eps_ip` 作为所有 epsilon 验证表的事实来源。

## 迁移计划

1. 增加或确认 epsilon 验证所需的 benchmark 输出字段。
2. 按照不同 `epsilon-percentile` 重建 `coco_100k` 的 `nlist=2048` 索引。
   统一落盘到 `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_eps{epsilon-percentile}`，例如 `index_fkmeans_2048_eps0.90`、`index_fkmeans_2048_eps0.95`、`index_fkmeans_2048_eps0.99`。
3. 用同一套 warm-serving 参数在每个重建索引上跑 benchmark。
4. 把结果和候选流统计写回 tracker 和 analysis 文档。
5. 根据结果决定下一步是否继续：
   - epsilon 调优 / 刷新 Pareto
   - 或者进入 CPU 路径实现

回滚方式：
- 删除 epsilon 验证专用的重建索引，回到当前最佳索引目录。
- 不需要回滚服务路径代码，因为本次变更只是文档和验证计划。

## 未决问题

- 第一轮验证是否要把 `epsilon-samples` 也作为次级变量一起扫，还是固定为当前默认值？
- 第一轮只用 `alpha=0.02` 是否足够，还是最终验证还要加入 `alpha=0.01` 作为高召回压力点？
- benchmark 是否需要直接输出一个派生的“Stage 2 输入数量”字段，还是由后处理根据现有计数计算？
