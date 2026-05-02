# Round 4 Refinement: Outline, Top-k Metrics, and Ablation Depth

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 主实验至少完成 `coco_100k` 与 `msmarco_passage`；`deep1m_synth` 作为第三数据集增强项，若时间不足则做合成 payload fallback；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出 COCO100K 与 MS MARCO 上的主实验表和 recall/quality-latency 曲线，完成关键机制 ablation，并给出可写进毕业论文的方法、创新点和章节大纲；Deep1M_synth 若完成则增强普适性，否则不阻塞主论文。

## Anchor Check

- Original bottleneck: 需要保留多数据集、多 baseline 的学术支撑，同时不能让实验矩阵失控。
- Why the revised method still addresses it: 本轮没有新增方法模块，只调整论文结构、top-k 指标层级和 ablation 设计。
- Reviewer/user suggestions rejected as drift: 不把 top20/top50 全部纳入所有 dataset x backend x nprobe 主矩阵；这会把实验量放大太多。

## Simplicity Check

- Dominant contribution after revision: Bound-guided selective fetch + dynamic scheduling。
- Components removed or merged: 章节大纲中删除“不启用 CRC/RAIR 的边界”作为方法章小节；该内容移到讨论/局限。
- Why the remaining mechanism is still small: ablation 增强围绕同一核心机制，不引入新 baseline 家族或新算法。

## Decisions

### 1. 章节结构

- 第 3 章不应放“COCO 与 MS MARCO 的 workload 差异”作为主小节。第 3 章应保留问题定义、系统瓶颈和动机实验。
- 数据集差异应挪到第 6 章实验设置，因为它属于 workload 描述与评估设计。
- 第 4 章不要出现“不启用 CRC early stop / RAIR 的边界”。方法章只写本文做了什么；CRC/RAIR 不做的原因放到第 8 章讨论与局限。

### 2. top-k 指标

- 主指标仍用 `topk=10` / `recall@10`，便于和现有 `bench_e2e` 输出、MS MARCO `MRR@10`、常见 dense retrieval 报告口径对齐。
- `topk=20` 应作为主文补充 operating point：它能验证方法在更大返回集合下是否仍保持 E2E 优势。
- `topk=50` 更适合作为 appendix / stress test：它会显著改变 payload fetch 数量、rerank 压力和 tail latency，不应进入所有主矩阵。
- 当前代码输出 recall 固定为 `@1/@5/@10`。若正式报告 `recall@20/@50`，需要扩展 `bench_e2e` 的 `recall_K` 与 JSON/CSV 输出，或用保存的 per-query predictions 做后处理。

### 3. ablation 深度

现有 ablation 方向正确但过粗。应分成三层：

- Core mechanism ablation: 三分类边界对 rerank pruning 和 payload prefetch 的影响。
- Scheduling ablation: 动态提交策略对 wait/tail latency 的影响。
- Stress/sensitivity ablation: top-k、payload size、threshold 对方法收益的影响。

## Revised Proposal Patch

本轮已将以下内容合并进 `FINAL_PROPOSAL.md`：

1. Metrics 增加 `topk=10` 主指标、`topk=20` 主文补充、`topk=50` appendix stress test。
2. Claim 2 ablation 扩展为 SafeOut pruning、SafeIn prefetch、Uncertain payload delay、boundary width / threshold sensitivity、Stage2 reclassification。
3. 第 3/4/6/8 章大纲重排：workload 差异移至第 6 章；CRC/RAIR 边界移至第 8 章。
