# Round 1 Review

<details>
<summary>Raw reviewer response</summary>

总评：这份 proposal 已经做对了最关键的一步：主动砍掉 CRC、RAIR、重复聚类和大而全 baseline 矩阵，问题锚点基本稳住了。但它还没有完全收口成一个“本科论文可按期完成的系统故事”。现在最大的风险不是实验不够，而是还残留着一个“小型 formal study”的影子：方法定义还不够硬，主 claim 还偏散，实验面仍然偏大。

我会先给结论：**方向对，范围仍需再压一刀；适合写成毕业论文，但还不适合直接按现稿推进。**

**评分**

| 维度 | 分数 | 简评 |
|---|---:|---|
| Problem Fidelity | 8 | 锚点清楚，non-goals 写得好，但 3 数据集 + 4 组合 + Lance 仍有扩张冲动。 |
| Method Specificity | 6 | 讲清了模块，没完全讲清“query plan 到底如何决定何时读什么、读多少、如何公平对比”。 |
| Contribution Quality | 6 | 主贡献方向是对的，但现在像“已有路径打包 + benchmark protocol”，新意边界还不够锋利。 |
| Frontier Leverage | 7 | 把问题放到 modern embedding retrieval serving 链路里是成立的，也没有乱加 LLM/RL。 |
| Feasibility | 6 | COCO 主闭环可做，但 `msmarco_passage`、`deep1m_synth`、Lance 复用都在消耗排期弹性。 |
| Validation Focus | 5 | 仍然过宽。当前矩阵更像“想证明很多事”，不是“最小但完整地证明一件事”。 |
| Venue/Thesis Readiness | 6 | 章节结构像系统论文，但中心结果和因果 ablation 还需要更强压缩。 |

**OVERALL SCORE: 6.4 / 10**

**低于 7 分的维度**

`Method Specificity = 6`
- 具体弱点：方法还是“组件列表”，不是“可判定执行策略”。尤其缺 3 个冻结点：候选预算如何定、异步 fetch 在哪一时刻发起、split baseline 用 top-k 还是 top-C 进入 payload fetch。
- 方法层面的修复：把方法写成固定 protocol。建议明确：
  1. 每 query 固定候选预算 `C`；
  2. 在 probed clusters 内完成近似打分后，针对 top-`C` 发起异步读取；
  3. exact rerank 只在这 `C` 个候选上进行；
  4. split baseline 也使用相同 `C` 或相同 recall target，比的是执行路径而不是偷换候选规模。
- 优先级：`CRITICAL`

`Contribution Quality = 6`
- 具体弱点：贡献 1 和 3 有价值，但贡献 4 “benchmark protocol” 不应占据贡献位；否则主贡献会从“集成执行路径”滑向“做了个实验包”。
- 方法层面的修复：把贡献压成“1 个主贡献 + 1 个支撑贡献”。
  - 主贡献：single-assignment IVF 下的 integrated retrieve-fetch-rerank query plan。
  - 支撑贡献：一个 matched-recall 的最小复现实验 protocol。
  - 题目建议优先用 `BoundFetch-Single` 或 `BoundFetch-Lite`，避免让读者期待 CRC/RAIR 家族式方法。
- 优先级：`IMPORTANT`

`Feasibility = 6`
- 具体弱点：主线仍依赖 3 个高风险项：Lance runner 可复用性、`deep1m_synth` 生成、MS MARCO 数据准备。
- 方法层面的修复：建立三级落地顺序。
  1. Mandatory：`COCO100K + FlatStor + 2 search kernels`
  2. Second：`deep1m_synth` 只做 1-2 个 payload size
  3. Optional appendix：Lance 或 `msmarco_passage`
- 优先级：`CRITICAL`

`Validation Focus = 5`
- 具体弱点：3 数据集 × 4 baseline 组合 × 多个 `nprobe` × 多个 ablation，仍明显过大。
- 方法层面的修复：把主实验缩成下面这个版本就够写论文：
  1. 数据：`COCO100K` 主实验，`deep1m_synth` 控制变量扩展；先删 `msmarco_passage`
  2. baseline：`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`
  3. operating points：2 个 recall target 或 3 个 `nprobe` 点，不要 5 点 sweep
  4. ablation：只保留 `overlap on/off` 和 `payload size sensitivity`
- 优先级：`CRITICAL`

`Venue/Thesis Readiness = 6`
- 具体弱点：结构像 VLDB/PVLDB，但“论文中最值得记住的一张图、一张表、一个因果 ablation”还没冻结。
- 方法层面的修复：按系统论文最小骨架收束：
  1. 主表：matched recall 下的 E2E latency
  2. 主图：COCO recall-latency
  3. 因果图：overlap on/off 的 latency breakdown
  4. 扩展图：payload size sensitivity
- 优先级：`IMPORTANT`

**Simplification Opportunities**
1. 删掉 `msmarco_passage` 主线地位，保留 `COCO100K + deep1m_synth` 即可。
2. 把 `Lance` 从主 baseline 降到 appendix / backup，只要 FlatStor 主线闭环完整，论文就能成立。
3. 把 Claim 3 降级为“workload sensitivity sanity check”，不要和 Claim 1/2 并列成主贡献。

**Modernization Opportunities**
1. 用 “retrieve -> fetch -> rerank serving chain” 作为全文 framing，比泛泛讲“向量数据库”更贴近当代系统问题。
2. 主指标优先强调 `p95/p99 E2E latency`，这比再加更多平均值或更多 dataset 更像现代 serving 论文。
3. 用 matched-recall operating point 取代大 sweep，会更像成熟系统论文的验证口径。

**Drift Warning**
- 当前最明显的 drift 是：把 `Lance`、`msmarco_passage`、多 storage backend、多 submission mode 一起推成主实验。这会从“证明 integrated execution plan 的必要性”漂移成“做一个缩小版 benchmark study”。

**Verdict**
`REVISE`

一句话建议：**保留题眼“集成执行路径降低 E2E latency”，再砍掉一个数据集、一个存储后端、一半 ablation，你的 proposal 就会从“方向合理”变成“可以按期写成像样毕业论文”。**

</details>
