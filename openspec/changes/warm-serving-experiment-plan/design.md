## Context

当前 BoundFetch 已完成 baseline 对比和一轮关键优化，最新 warm 结果表明：

- BoundFetch 在 `coco_100k` 上达到约 `1.136ms @ recall@10=0.898`
- DiskANN+FlatStor 的对应 warm 点约为 `3.790ms @ recall@10=1.000`
- `io_wait_ms` 已接近 0，主要成本转移到 `uring_submit_ms` 和 `probe_ms`
- `io_queue_depth` 继续增大没有稳定收益
- `SQPOLL` 在当前环境回退，不能作为主收益来源

同时，服务器不提供 `sudo`，且项目方明确表示 cold-start 不是现实生产环境下的核心目标。因此后续设计必须把实验协议、验收标准和执行顺序全部收束到 warm steady-state。

## Goals / Non-Goals

**Goals:**
- 定义唯一主协议：warm steady-state
- 让后续实验直接服务于两件事：
  - BoundFetch 的 warm E2E Pareto 是否成立
  - 是否还能在接近当前延迟下显著提升 recall
- 固定最小执行顺序和停机门槛，避免继续扩张为大而全 benchmark 套件
- 将已有 qd / mode / SQPOLL 结果降级为附录或冻结项

**Non-Goals:**
- 不再要求 cold-start / drop-cache / semi-cold
- 不重新展开超大数据集 sweep 作为主线
- 不把更多低层 `io_uring` 微优化作为主任务
- 不把 SafeIn 激活条件探索作为主结果中心

## Decisions

### 1. 以 warm-only 取代 cold/warm 双协议

选择：
- 所有主结果、主表、主图只使用 warm steady-state

原因：
- 与部署现实一致
- 与当前权限环境一致
- 避免继续为不服务于最终论文故事的协议投入工程时间

备选方案：
- 保留 cold 作为“理想附录协议”
- 否决原因：会持续制造未完成负担，也会稀释主故事

### 2. 主结果采用 Pareto-first，而不是单点对比

选择：
- 用 BoundFetch 的多参数 sweep 与两条强 baseline 的参数 sweep 一起构成三条 recall-latency Pareto

原因：
- 当前最主要的争议不在“能不能更快”，而在“更快是否只是用 recall 换来的”
- Pareto 比单点更能支撑论文主张
- 用户已经观察到 COCO 上单独增加 `nprobe` 会受到 CRC 早停影响，边际收益渐消，因此只扫 `nprobe` 不足以描述真实可调空间
- 最新调优已经表明 `crc-alpha` 是当前最有效的主要控制轴，而 `epsilon-percentile` 在 `nprobe>=200` 区间影响很弱，因此后续 sweep 应围绕“先对齐三条 Pareto，再补充必要验证”展开

备选方案：
- 继续只保留最佳单点
- 否决原因：容易被 reviewer 质疑选点偏置

### 3. 将机制归因单独做成最小支撑块

选择：
- 用 `uring_submit_ms / probe_ms / io_wait_ms / submit_calls` 构成一张主归因图或表

原因：
- 现在需要解释“为什么快”
- 但不能让整篇工作退化成 profiling note

备选方案：
- 展开更多 perf / kernel tracing
- 否决原因：投入大、收益小、容易转移论文重心

### 4. 把后续代码优化门槛改成 recall-improvement gate

选择：
- 仅当小规模阈值/策略调整能在近似延迟下显著提高 recall 时，继续优化代码

原因：
- 当前 submit-path 已经吃掉大部分显性收益
- 继续做系统微优化的边际回报不明

备选方案：
- 持续做 deeper submit-path / qd / SQPOLL 优化
- 否决原因：已有实验已经显示这些方向不是当前主要收益点

### 5. 把 change 文档写成“可执行 runbook”，而不是抽象计划

选择：
- 在本 change 中直接固定实验矩阵、输出路径、验收门槛和文档回填顺序

原因：
- 当前阶段最需要的是执行清晰度，而不是再多一层抽象
- 用户希望拿着文档即可跑实验并回填结果

备选方案：
- 继续只保留高层规划
- 否决原因：会导致后续实现时仍需要二次解释和拆解

## Execution Blueprint

### Block A: 主结果 Pareto（必须先做）

**目标**
- 形成 COCO 100K warm steady-state 下的主 Pareto 曲线
- 让 BoundFetch 与现有 baseline 在同一张图和同一张表里可直接比较

**固定设置**
- dataset: `coco_100k`
- queries: `1000`
- topk: `10`
- bits: `4`
- io_queue_depth: `64`
- submission_mode: `shared`
- protocol: `warm`

**BoundFetch 主参数矩阵**
- 主轴固定为 `nprobe={200,500}`
- 主调参数固定为 `crc-alpha={0.01,0.02,0.05,0.08,0.1,0.15,0.2}`
- 次调参数固定为 `epsilon-percentile={0.99,0.95,0.9}`
- 结构参数固定为 `nlist={1024,2048}`

**BoundFetch 执行裁剪规则**
- 不执行四维全笛卡尔积
- 第 1 步：固定 `nlist=2048`、`epsilon-percentile=0.99`，完整扫 `nprobe x crc-alpha`
- 第 2 步：仅在每个 `nprobe` 的前 2 个候选 `crc-alpha` 上补 `epsilon-percentile={0.95,0.9}`
- 第 3 步：只对最终候选点补 `nlist=1024` 对照
- 如果 `epsilon-percentile` 再次表现为不敏感，则后续主表只保留 `0.99`
- 所有保留点共同构成一条 BoundFetch Pareto 曲线，而不是只来自单一 `nprobe` sweep

**Baseline 参数矩阵**
- `DiskANN-disk + FlatStor-sim`
  - 主调参数：`L_search={32,64,96,128,160,192,256}`
  - 如实现中存在更稳定的候选集扩张参数，可记录在 `notes`，但主曲线按 `L_search` 组织
- `FAISS-IVFPQ-disk + FlatStor-sim`
  - 主调参数：`nprobe={8,16,32,64,128,256,512}`
  - 若高档 `nprobe` 已触发明显 recall ceiling，也必须保留这些点以展示上限
- 两个 baseline 都必须各自形成一条独立 Pareto 曲线，而不是只挑单点陪跑

**比较要求**
- 主图中至少出现三条曲线：
  - BoundFetch
  - DiskANN+FlatStor
  - FAISS-IVFPQ-disk+FlatStor
- 曲线对比目标是 end-to-end `search + payload`
- 若 FAISS 在 COCO 上受 PQ 上限限制，也必须保留其曲线，并在分析中明确其 recall ceiling

**输出要求**
- 主结果追加到 `baselines/results/e2e_comparison_warm.csv`
- 记录字段至少包括：
  - `system`
  - `dataset`
  - `topk`
  - `param`
  - `recall@10`
  - `vec_search_ms`
  - `payload_ms`
  - `e2e_ms`
  - `notes`
  - `protocol`

**可执行实验表**

| Batch | System | Fixed Settings | Sweep | Expected Runs | Primary Goal | Output |
|------|--------|----------------|-------|---------------|--------------|--------|
| A1 | BoundFetch | `dataset=coco_100k`, `queries=1000`, `topk=10`, `bits=4`, `qd=64`, `mode=shared`, `nlist=2048`, `epsilon=0.99` | `nprobe={200,500}` x `crc-alpha={0.01,0.02,0.05,0.08,0.1,0.15,0.2}` | 14 | 先锁定 BoundFetch 主 Pareto 骨架 | CSV + tracker |
| A2 | BoundFetch | 同 A1，仅保留每个 `nprobe` 最优前 2 个 `crc-alpha` | `epsilon={0.95,0.9}` | 4-8 | 验证 epsilon 是否值得保留为主参数 | CSV + analysis |
| A3 | BoundFetch | 固定最终候选点 | `nlist={1024,2048}` | 2-4 | 判断 `nlist` 是否进入主图 | CSV + analysis |
| A4 | DiskANN+FlatStor | `dataset=coco_100k`, `queries=1000`, `topk=10` | `L_search={32,64,96,128,160,192,256}` | 7 | 构造 DiskANN baseline Pareto | CSV + tracker |
| A5 | FAISS-IVFPQ+FlatStor | `dataset=coco_100k`, `queries=1000`, `topk=10`, `bits=4`, `nlist` 与 BoundFetch 主设置对齐 | `nprobe={8,16,32,64,128,256,512}` | 7 | 构造 FAISS baseline Pareto | CSV + tracker |

**主图筛点规则**
- 每条曲线至少保留 4 个非支配点
- 若某点在 `recall@10` 和 `e2e_ms` 上同时劣于同族其他点，则仅保留在原始 CSV，不进入主图
- 主图中 BoundFetch 默认优先展示 `nlist=2048`，仅当 `1024` 出现新的非支配点时才加入
- baseline 不允许只保留“最佳单点”，必须保留从低延迟到高召回的完整代表段

**验收门槛**
- 必须形成三条可用于主图的 Pareto 曲线
- BoundFetch 曲线必须包含多参数调节产生的代表点，而不是只体现 `nprobe` 单轴变化
- baseline 曲线必须来自各自参数 sweep，而不是只保留历史单点

### Block B: 机制归因（在 Pareto 稳定后做）

**目标**
- 解释当前最优点为什么快
- 把 `submit/probe/io_wait` 的角色固定下来，避免后续继续误投低层优化

**固定设置**
- 主点：当前最优工作点，优先 `nprobe=200, crc-alpha=0.05, epsilon=0.99, nlist=2048`
- 对照点：从同一 `nprobe` 下的相邻 `crc-alpha` 候选中选择 1 个，再从 `nprobe=500` 的代表点中选择 1 个

**输出要求**
- 每个点至少记录：
  - `uring_submit_ms`
  - `probe_ms`
  - `parse_cluster_ms`
  - `rerank_ms`
  - `io_wait_ms`
  - `submit_calls`
- 结果应回填到 `baselines/results/analysis.md`

**验收门槛**
- 必须明确说明 `io_wait` 已不是主瓶颈
- 必须明确说明继续提升 qd / SQPOLL 不是当前主线

### Block C: 最小 Recall 提升消融（决定是否继续改代码）

**目标**
- 判断是否还能在近似当前延迟下继续提升 recall

**参数范围**
- 变量集只允许一次改变一类机制：
  - `CRC_alpha` 主 sweep 已固定为 `{0.01,0.02,0.05,0.08,0.1,0.15,0.2}`
  - `epsilon-percentile` 固定为 `{0.99,0.95,0.9}`
  - `nlist={1024,2048}` 的代表点比较
  - 只有在以上三轴都无法提供新非支配点时，才允许补 pruning / verification threshold sweep

**固定设置**
- 仍使用 `coco_100k`
- 仍使用 `queries=1000`
- 固定 `io_queue_depth=64`
- 固定 `submission_mode=shared`

**输出要求**
- 每个变体写入统一结果表或独立附录表
- 必须记录：
  - `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - `uring_submit_ms`
  - `submit_calls`

**停止条件**
- 若没有任何变体能在小于约 `10-15%` 延迟增幅下提升 recall，则冻结代码优化主线
- 若有变体能显著抬高 recall，则可以开新的优化 change，专门做 recall-path 改进
- 在当前已知结果下，默认先完成 baseline 两条 Pareto，再决定是否继续 CPU/IO 协同优化

## Handoff Table

| Order | Owner View | Task | Inputs | Command Template / Action | Completion Standard | Backfill |
|------|------------|------|--------|----------------------------|---------------------|----------|
| 1 | Runner | 执行 BoundFetch A1 骨架 sweep | `coco_100k` 已有索引 | 运行 `bench_e2e`，固定 `queries=1000`，遍历 `nprobe={200,500}` 和 `crc-alpha` 7 档 | 得到 14 个 warm 点且 CSV 无缺列 | `e2e_comparison_warm.csv`, `EXPERIMENT_TRACKER*.md` |
| 2 | Analyst | 从 A1 中选每个 `nprobe` 的前 2 个候选 alpha | A1 结果 | 按非支配排序和“低延迟优先”筛点 | 每个 `nprobe` 至少 2 个候选进入 A2 | `analysis.md` |
| 3 | Runner | 执行 BoundFetch A2 epsilon 验证 | A2 候选点 | 固定其它参数，仅扫 `epsilon={0.95,0.9}` | 明确 epsilon 是否产生新非支配点 | `e2e_comparison_warm.csv`, `analysis.md` |
| 4 | Runner | 执行 BoundFetch A3 nlist 对照 | A2/A1 最终候选 | 在 `nlist={1024,2048}` 间切换 | 明确主图是否需要两种 nlist | `analysis.md`, tracker |
| 5 | Runner | 执行 DiskANN A4 sweep | baseline 可执行脚本 | 遍历 `L_search={32,64,96,128,160,192,256}` | 至少形成 4 个可比较工作点 | CSV, tracker |
| 6 | Runner | 执行 FAISS A5 sweep | baseline 可执行脚本 | 遍历 `nprobe={8,16,32,64,128,256,512}` | 至少形成 4 个可比较工作点 | CSV, tracker |
| 7 | Analyst | 生成三条 Pareto 主图和选点表 | A1-A5 全部结果 | 去除支配点，统一绘图与表格 | 三条曲线和主表可直接用于论文草稿 | `analysis.md`, figure plan |
| 8 | Lead | 做下一步 go/no-go 决策 | 主图 + 机制归因 | 若 baseline 已对齐，优先决定是否继续 recall-path；CPU/IO 优化后置 | 形成后续 change 结论 | `FINAL_PROPOSAL*.md` 或新 change |

### Block D: 附录冻结项（最后做）

**内容**
- qd sweep
- shared vs isolated
- SQPOLL fallback

**要求**
- 不再扩展参数组合
- 只整理已有结果，除非环境发生实质变化

## Risks / Trade-offs

- [BoundFetch 仍然低于强 baseline 的高 recall 区域] → 必须优先完成 recall-improvement ablation，并用 Pareto 而非单点描述结果
- [论文被看成“调参日志”] → 把 qd / mode / SQPOLL 固定为 appendix supporting evidence，不继续扩张
- [只有 COCO 主数据集会被质疑泛化不足] → 在主结果站稳后，用 Deep1M 或 Deep8M 做轻量支撑，不做全套 sweep
- [warm-only 会被质疑规避最难场景] → 在 proposal 和 spec 中明确 cold-start 是 non-goal，且系统目标是 production-style steady-state serving

## Migration Plan

1. 更新变更文档与实验计划，统一为 warm-only，并固定 `queries=1000`
2. 先补齐 BoundFetch 主参数骨架，再同步补两条 baseline 曲线
3. 完成三条 Pareto 后，再做最小机制归因与是否继续代码优化的 gate
4. 每完成一个 block，就同步回填：
   - `baselines/results/e2e_comparison_warm.csv`
   - `baselines/results/analysis.md`
   - `refine-logs/EXPERIMENT_TRACKER*.md`
5. 若 baseline 对齐后 BoundFetch 仍有明显空白区间，再继续 recall-path 消融
6. 若 recall 没有改善空间，则冻结代码，转入写作和图表整理
7. 若 recall 可以改善，再决定是否开新的优化 change；CPU/IO 协同优化排在 baseline 对齐之后

## Open Questions

- 在 baseline sweep 完成后，DiskANN 是否需要额外引入构图/缓存相关注释字段来解释异常点？
- 若 FAISS 曲线在 `nprobe=128` 后已完全饱和，是否还需要保留 `256/512` 两点进入主图，还是仅保留在原始 CSV？
- Deep1M 与 Deep8M 中，哪个更适合作为 appendix generality check？
