# 结果分析：BoundFetch-Guarded 论文实验

**日期**: 2026-05-04  
**参考蓝图**: `idea-stage/THESIS_BLUEPRINT.md`  
**范围**: 主实验 T000-T015、机制消融 T016-T019、调度消融 T021-T023、top-k 补充 T024。  
**主要数据源**:

- `/home/zcq/VDB/baselines/formal-study/outputs/e2e/coco_100k/e2e_system_summary.csv`
- `/home/zcq/VDB/baselines/formal-study/outputs/e2e/msmarco_passage/e2e_system_summary.csv`
- `/home/zcq/VDB/baselines/formal-study/outputs/ablation_summaries/triage_ablation_summary.csv`
- `/home/zcq/VDB/baselines/formal-study/outputs/ablation_summaries/scheduling_ablation_summary.csv`
- `/home/zcq/VDB/baselines/formal-study/outputs/ablation_summaries/topk20_sensitivity_summary.csv`
- `RESULT_FREEZE_ABLATION.md`

## 执行摘要

实验结果支持当前本科论文主线：**BoundFetch-Guarded 在 COCO100K 和 MS MARCO 上，以更低端到端延迟达到与最强 FlatStor baseline 相近或更高的 recall@10；机制消融显示收益主要来自 SafeOut 剪枝和 Uncertain 延迟取数；调度消融显示 submit batch 的合理选择能进一步改善尾延迟。**

论文结论建议按如下精确表述：

- 核心结论成立：BoundFetch-Guarded 在两个数据集上都优于 `IVF+RaBitQ+FlatStor`，这是最有对比意义的 baseline。
- `IVF+PQ` 在两个数据集上的 recall 上限明显偏低，适合作为弱 baseline 和 PQ 对照，不适合作为严格匹配质量（matched high-quality）的对比基线。
- SafeOut 是最强机制贡献：COCO 上关闭 SafeOut 后 rerank candidates 增加 13.56 倍，平均延迟增加 5.79 倍，p99 增加 5.23 倍。
- SafeIn prefetch 在当前 working point 上不是主要收益来源；COCO 中关闭 SafeIn prefetch 反而略快，MS MARCO 中影响也很小。论文应把 SafeIn 写成“低成本提前 materialization 的机会”，不要写成主要加速来源。
- Uncertain delayed payload 是稳定收益来源：COCO 上 eager payload 使 p99 增加 15.5%，MS MARCO 上增加 28.0%。
- top20 补充支持更大 top-k 下的稳定性：COCO 与 MS MARCO 中 BoundFetch top20 均在更高 recall 下保持更低 E2E latency。

## 原始结果表

### 主要结果：COCO100K 高 recall 匹配点

COCO 的论文主表建议选取 `Q*=0.95` 附近的高 recall 区域。重复且含噪声的 BoundFetch 点已剔除，使用后续 sweep 中的稳定测量结果。

| 系统 | 后端 | nprobe | recall@10 | 平均 e2e (ms) | p95 (ms) | p99 (ms) | 选点说明 |
|---|---|---:|---:|---:|---:|---:|---:|
| BoundFetch-Guarded | integrated/FlatStor | 64 | 0.9582 | 1.1307 | 1.4178 | 1.5862 | matched |
| IVF+RaBitQ | FlatStor | 256 | 0.9701 | 2.2447 | 2.3231 | 2.9198 | matched |
| IVF+PQ | FlatStor | 256 | 0.7445 | 3.5636 | 3.6284 | 5.9531 | 参考（质量低于 Q*） |
| IVF+RaBitQ | Lance | 256 | 0.9701 | 10.5382 | 9.8053 | 10.6120 | 后端参考 |

### 主要结果：MS MARCO 中 recall 中高区匹配点

MS MARCO 的主表建议使用 `Q*=0.90`。`nprobe=256` 下 BoundFetch 虽有更高 recall，但尾延迟异常噪声明显，不宜作为主尾延迟证据。

| 系统 | 后端 | nprobe | recall@10 | 平均 e2e (ms) | p95 (ms) | p99 (ms) | 选点说明 |
|---|---|---:|---:|---:|---:|---:|---:|
| BoundFetch-Guarded | integrated/FlatStor | 128 | 0.9320 | 9.0271 | 12.9516 | 15.4166 | matched |
| IVF+RaBitQ | FlatStor | 128 | 0.9246 | 63.9502 | 73.8637 | 90.9700 | matched |
| IVF+PQ | FlatStor | 1024 | 0.6472 | 149.1369 | 150.4445 | 152.7809 | 参考（质量低于 Q*） |
| IVF+RaBitQ | Lance | 128 | 0.9247 | 42.9723 | 51.3430 | 68.0894 | 后端参考 |

### BoundFetch Recall-Latency 曲线

| 数据集 | nprobe | recall@10 | 平均 e2e (ms) | p99 (ms) | 说明 |
|---|---:|---:|---:|---:|---|
| COCO100K | 16 | 0.8304 | 0.6720 | 1.0097 | 稳定 |
| COCO100K | 32 | 0.9127 | 0.8247 | 1.2318 | 稳定 |
| COCO100K | 64 | 0.9582 | 1.1307 | 1.5862 | 高 recall 主选点 |
| COCO100K | 128 | 0.9835 | 1.6888 | 2.2695 | 稳定 |
| COCO100K | 256 | 0.9947 | 2.7669 | 3.5928 | 稳定 |
| COCO100K | 512 | 0.9986 | 4.5459 | 5.7343 | 稳定 |
| MS MARCO | 16 | 0.7786 | 7.3108 | 17.9266 | 稳定 |
| MS MARCO | 32 | 0.8443 | 7.2875 | 14.5896 | 稳定 |
| MS MARCO | 64 | 0.8934 | 9.7796 | 30.4943 | 尾延迟偏高 |
| MS MARCO | 128 | 0.9320 | 9.0271 | 15.4166 | 主对比点 |
| MS MARCO | 256 | 0.9603 | 32.8081 | 304.4682 | 尾延迟噪声；不用于主 p99 证据 |
| MS MARCO | 512 | 0.9763 | 13.8631 | 26.3463 | 去除异常点后稳定 |
| MS MARCO | 1024 | 0.9895 | 20.0816 | 37.2204 | 稳定 |

### 机制消融原始表

COCO 使用 `topk=10,nprobe=64`；MS MARCO 使用 `topk=10,nprobe=128` 进行跨数据集复验。

| 数据集 | 变体 | recall@10 | 平均 e2e (ms) | p95 (ms) | p99 (ms) | reranked candidates/query | SafeIn payload prefetched/query | final payload fetches/query |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| COCO100K | Full | 0.9582 | 1.2818 | 1.6449 | 1.8135 | 232.293 | 0.109 | 9.913 |
| COCO100K | SafeIn prefetch off | 0.9582 | 1.1836 | 1.5148 | 1.6718 | 232.293 | 0.000 | 10.000 |
| COCO100K | Uncertain eager payload | 0.9582 | 1.3996 | 1.8417 | 2.0944 | 232.293 | 232.293 | 0.000 |
| COCO100K | SafeOut pruning off | 0.9582 | 7.4248 | 8.9294 | 9.4914 | 3149.210 | 0.109 | 9.913 |
| MS MARCO | Full | 0.9320 | 9.0816 | 13.2181 | 15.5815 | 676.982 | 0.322 | 9.839 |
| MS MARCO | SafeIn prefetch off | 0.9320 | 9.1294 | 13.3086 | 15.8043 | 676.982 | 0.000 | 10.000 |
| MS MARCO | Uncertain eager payload | 0.9320 | 11.0217 | 16.7117 | 19.9504 | 676.982 | 676.982 | 0.000 |

### 调度消融原始表

所有调度消融均使用 COCO100K `topk=10,nprobe=64`。

| 变体 | recall@10 | 平均 e2e (ms) | p95 (ms) | p99 (ms) | submit calls/query | submit flushes/query |
|---|---:|---:|---:|---:|---:|---:|
| submit_online=0 | 0.9582 | 1.3016 | 1.6633 | 1.8785 | 6.858 | 5.861 |
| submit_online=1 | 0.9582 | 1.3020 | 1.6547 | 1.8251 | 6.858 | 5.861 |
| submit_batch=4 | 0.9582 | 1.3332 | 1.7042 | 1.8992 | 20.150 | 19.153 |
| submit_batch=8 | 0.9582 | 1.2008 | 1.5097 | 1.7286 | 14.526 | 13.529 |
| submit_batch=16 | 0.9582 | 1.1860 | 1.5200 | 1.6887 | 10.107 | 9.110 |
| submit_batch=32 | 0.9582 | 1.3077 | 1.6730 | 1.8347 | 6.858 | 5.861 |

### Top20 补充实验原始表

| 数据集 | 系统 | 后端 | nprobe | recall@20 | 平均 e2e (ms) | p95 (ms) | p99 (ms) |
|---|---|---|---:|---:|---:|---:|---:|
| COCO100K | BoundFetch-Guarded | integrated/FlatStor | 64 | 0.9529 | 1.5641 | 2.0372 | 2.3795 |
| COCO100K | IVF+RaBitQ | FlatStor | 64 | 0.8109 | 1.7505 | 1.6430 | 2.5890 |
| COCO100K | IVF+PQ | FlatStor | 64 | 0.7500 | 2.1793 | 2.1460 | 2.4616 |
| MS MARCO | BoundFetch-Guarded | integrated/FlatStor | 128 | 0.9278 | 11.3763 | 17.4967 | 20.0412 |
| MS MARCO | IVF+RaBitQ | FlatStor | 128 | 0.9193 | 64.3284 | 80.5912 | 94.8355 |
| MS MARCO | IVF+PQ | FlatStor | 128 | 0.6351 | 69.5689 | 72.6233 | 85.1618 |

## 关键发现

### 发现 1：BoundFetch-Guarded 在 matched-quality 比较中胜出

**观察**: COCO100K 在高 recall 时，BoundFetch 以 recall@10 为 0.9582、平均 1.1307ms 达到目标，IVF+RaBitQ+FlatStor 为 recall@10 0.9701、平均 2.2447ms；对应平均延迟加速 `1.99x`、p99 加速 `1.84x`。MS MARCO 在 `Q*=0.90` 时，BoundFetch 达到 recall@10 0.9320、9.0271ms，IVF+RaBitQ+FlatStor 为 recall@10 0.9246、63.9502ms；对应平均延迟加速 `7.08x`、p99 加速 `5.90x`。

**解读**: 差异不只是 recall 质量伪差异。最强对比 baseline IVF+RaBitQ 在 recall 上接近时，仍需承担 split vector-search 与 payload 读取路径的拆分成本，而 BoundFetch 通过将 triage、vector 取数、rerank 与 payload 读取整合为一条执行路径显著降低 E2E 成本。

**含义**: 直接支持论文 RQ1，应放在第 7 章第一节。

**论文表述**: 在匹配 recall@10 下，BoundFetch-Guarded 相比 IVF+RaBitQ+FlatStor 在 COCO100K 上将平均 E2E 延迟降低 `1.99x`，在 MS MARCO 上降低 `7.08x`。

### 发现 2：IVF+PQ 在高 recall 区间是弱 baseline

**观察**: IVF+PQ+FlatStor 在 COCO 和 MS MARCO 的最佳保留配置下，recall@10 分别仅为 0.7445 和 0.6472，均不能在高 recall 区间与 BoundFetch / IVF+RaBitQ 匹配。

**解读**: PQ 仍可作为传统压缩向量对照，但不是本论文中最强质量匹配竞争对手；更合理的主要对比是 BoundFetch vs IVF+RaBitQ（同属 RaBitQ 量化族）。

**含义**: 主表中 PQ 行应标记为“best effort”，避免在高 recall 下强行做匹配速度比。

### 发现 3：Lance 适合放后端参考，主线仍以 FlatStor 为主

**观察**: 在 COCO 的高 recall 点，IVF+RaBitQ+Lance 的平均延迟为 10.5382ms（与 BoundFetch 1.1307ms 对比）；MS MARCO 的对应点为 42.9723ms（与 BoundFetch 9.0271ms 对比）。

**解读**: Lance 在本测量下增加了更高的 payload 访问开销。它对展示 backend 敏感性有价值，但不应把论文引向 Lance-vs-FlatStor 的存储对比。

**含义**: 将 Lance 设为 backend benchmark 小节。主线论点仍聚焦 BoundFetch-Guarded 的 candidate-aware fetch，而非存储后端论文。

### 发现 4：SafeOut 剪枝是核心机制贡献

**观察**: COCO 下关闭 SafeOut 后，recall@10 不变（0.9582），但平均延迟从 1.2818ms 增至 7.4248ms，p99 从 1.8135ms 增至 9.4914ms；rerank 候选从 232.293 增至 3149.21，每查询提升 `13.56x`。

**解读**: SafeOut 剪枝是主要功臣，能阻止低置信候选进入高代价 rerank/payload 读取路径。

**含义**: RQ2 中先突出 SafeOut，建议用删除式消融柱状图展示。

### 发现 5：Uncertain 延迟取数是稳健的次级机制

**观察**: COCO 上对 Uncertain 候选 eager 取数会使 p99 从 1.8135ms 升至 2.0944ms（+15.5%）；MS MARCO 上从 15.5815ms 升至 19.9504ms（+28.0%），两者 recall 不变。

**解读**: 大量 Uncertain 候选最终未入榜，提前取数会造成无效 materialization。延迟到 exact rerank 后再取数能稳定减少尾延迟。

**含义**: 支持论文的“payload-aware”观点，并且将本方案与只优化向量打分的系统区分开。

### 发现 6：SafeIn prefetch 需谨慎表述

**观察**: 在固定配置下，SafeIn prefetch 作用很小。COCO 关闭后平均与 p99 均略快；MS MARCO 关闭后 p99 仅上升 1.43%。SafeIn 预取次数分别为 0.109/query（COCO）与 0.322/query（MS MARCO）。

**解读**: 当前 bound/frontier 设定较保守，少量候选被判为 SafeIn。当前收益主要来自 SafeOut 剪枝和 Uncertain 延迟取数。

**含义**: 不要把 SafeIn prefetch 写成主要加速来源；将其作为 triage policy 完整性部分与低风险提前 materialization 的机会点呈现。

### 发现 7：submit batch 调优作用高于 submit_online

**观察**: 开启 submit_online 可将 p99 从 1.8785ms 降到 1.8251ms，改进 2.8%；submit_batch 的影响更显著：batch=16 的平均延迟（1.1860ms）和 p99（1.6887ms）均最好，batch=4 与 32 均更差。

**解读**: batch 太小会提交过于频繁，batch 太大又拖尾；中间值在 overhead 与及时性之间最优。

**含义**: RQ3 支持成立，但叙述应保守：调度是有意义的工程优化，核心贡献仍在 triage 机制。

### 发现 8：Top20 补充说明结果在更大返回规模下更稳健

**观察**: COCO top20 下，BoundFetch（nprobe64）达 recall@20 0.9529 且 1.5641ms，IVF+RaBitQ 为 0.8109 且 1.7505ms；MS MARCO top20 下 BoundFetch（nprobe128）为 0.9278、11.3763ms，IVF+RaBitQ 为 0.9193、64.3284ms。

**解读**: top-k 增大带来 payload 压力，但 BoundFetch 仍能控制 E2E；MS MARCO 改善更明显。

**含义**: top20 作为补充即可，top50/top100 可作为后续 appendix 压力测试。

### 发现 9：MS MARCO 的 nprobe=256 是尾延迟离群点

**观察**: MS MARCO 的 nprobe=256 BoundFetch 为 recall@10 0.9603、平均 32.8081ms，但 p99 达 304.4682ms；而邻近 nprobe=512 的 p99 为 26.3463ms。

**解读**: 更像测量/系统噪声而非算法趋势。tracker 中已记录该点并做了 hygiene 重跑。

**含义**: 不应将该点作为主表代表点。可在曲线上保留并标注异常，或 p99 图中剔除该点。

## 第 7 章建议组织

### 7.1 COCO100K 上的主结果

采用 COCO 的高 recall 匹配点与完整曲线。

- BoundFetch 更早达到高 recall。
- 与 IVF+RaBitQ+FlatStor 相比，在高 recall 区间平均延迟加速 `1.99x`、p99 `1.84x`。
- IVF+PQ 在质量上受限，应标记为 best effort。

### 7.2 MS MARCO 上的主结果

采用 `Q*=0.90` 与稳定的 nprobe=128 点。

- BoundFetch 达到 recall@10 0.9320，延迟 9.0271ms。
- IVF+RaBitQ+FlatStor 在接近 recall 下为 63.9502ms。
- 给出平均延迟加速 `7.08x`、p99 `5.90x`。

### 7.3 后端 benchmark：FlatStor 与 Lance

使用 Lance 行为后端参考：

- COCO：高 recall 下 IVF+RaBitQ+Lance 相比 BoundFetch 平均慢 `9.32x`。
- MS MARCO：相对慢 `4.76x`。
- 说明：后端决定性仍在本论文主线，但存储访问代价可显著抬升 E2E。

### 7.4 bound 指引 triage 消融

使用删除式消融表：

- SafeOut 关闭是决定性证据。
- Uncertain eager payload 为次强证据。
- SafeIn prefetch 当前为保守优化。

### 7.5 动态调度消融

使用 submit-batch 表：

- batch=16 是测得最优。
- submit-online 有小但正向 p99 效果。
- 将调度定位为次级而非主贡献。

### 7.6 Top-k 敏感性

正文仅保留 top20：

- COCO top20：BoundFetch 在更高 recall 下仍快于对照。
- MS MARCO top20：在近似同 recall 下 BoundFetch 仍比 IVF+RaBitQ+FlatStor 快 `5.65x`。
- top50/top100 保留为延期补充。

## 建议下一步

在当前本科阶段，不需要新增主实验。建议聚焦结果整理与汇报材料：

1. 从冻结 CSV 生成最终图表
   - COCO 与 MS MARCO 的 recall@10-vs-avg e2e
   - triage 删除式柱状图
   - submit-batch p99 曲线
   - top20 补充表或小曲线
2. 明确处理 MS MARCO `nprobe=256` 噪声点：
   - 标注为异常噪声，或
   - 在 p99 图中不作为代表点使用
3. 若有时间，只重跑图表脚本，不新增主/消融矩阵
4. 不扩展 top50/top100 的完整矩阵（除非论文需要 appendix 压力实验）

## 最终结论性表述

1. BoundFetch-Guarded 相比 `IVF+RaBitQ+FlatStor`，在 COCO100K 和 MS MARCO 的 matched-quality 下，平均 E2E 分别提升 `1.99x` 和 `7.08x`。
2. SafeOut 剪枝是核心机制，COCO 上关闭后 rerank 工作量增加 `13.56x`，p99 增加 `5.23x`，且 recall 不变，说明 bound-aware 剪枝是关键。
3. 对 Uncertain 候选延迟读取 payload 相比 eager，可将 p99 分别降低 15.5%（COCO）和 28.0%（MS MARCO）。
4. 当前配置下 SafeIn prefetch 影响有限，原因是安全入队样本很少。
5. submit-batch 调优有可测收益，batch=16 在测量范围内给出最优 p99。
6. top20 补充证明扩容到更大结果集时仍保持更低延迟与更高召回。
