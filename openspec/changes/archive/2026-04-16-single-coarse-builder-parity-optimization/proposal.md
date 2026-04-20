## Why

当前 single-mode coarse partition 的 high-recall 表现明显弱于 `IVF+RQ` / `IVF+RaBitQ` baseline，但现有证据还没有把根因精确隔离到“分层 SuperKMeans 路径本身”还是“当前 builder family 整体不如 Faiss IVF coarse quantizer”。在继续设计更复杂的 recall-aware clustering 之前，先按最小变量原则完成 single-builder parity 诊断：先验证单层 SuperKMeans 能否显著缩小 probe gap；若不能，再切到复用 Faiss IVF clustering，确认仅靠更强的传统 coarse builder 能否达到 baseline 级别的 probe 水平。

## What Changes

- 增加 single-mode coarse builder parity 工作流，明确比较 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 三条 single-assignment coarse partition 路径。
- 将当前算法改造为分阶段 gate：第一阶段先把现有分层 SuperKMeans 路径替换为单层 SuperKMeans，在相同 single 模式下重跑并判断 probe budget 是否显著改善。
- 若单层 SuperKMeans 仍不能显著缩小 high-recall gap，第二阶段切换到复用 Faiss IVF coarse clustering 的 single-assignment builder，并将其导出的 centroids / assignments 接入现有 IVF build 与 serving 对照流程。
- 增加 builder-parity 评估与验收标准，直接报告达到目标 `recall@10` 或等价 candidate-recall operating point 所需的最小 `nprobe`，并以 `IVF+RQ` / `IVF+RaBitQ` baseline 作为目标参考。
- 暂停 recall-aware overlap、RAIR、top-2 duplication 等 secondary-assignment 路线在本 change 中的推进；只有在 strong single-builder parity 仍无法缩小差距时，后续才重新考虑更复杂的 partition algorithm。

## Current Summary

- `coco_100k / nlist=2048 / single / topk=10 / target recall@10=0.99` 的 phase-1 对照已经完成：
  `hierarchical_superkmeans` 在 `nprobe<=384` 内未达到目标，最佳为 `0.9861@384`；单层 `superkmeans` 在 `nprobe=384` 达到 `0.9900`。这说明分层路径确实带来损失，但去掉分层后仍明显弱于 baseline operating point。
- phase-2 对照已经完成：
  复用 `faiss_kmeans` coarse clustering 后，当前方法达到 `recall@10=0.99` 的最小 `nprobe` 可降到约 `256`，已经回到 `IVF+RQ` / `IVF+RaBitQ` baseline 的量级。这把根因进一步收敛到 coarse builder 质量，而不是现有 query、candidate 或 rerank 主流程。
- 额外的纯 coarse 诊断也已经完成：
  在“99% 概率下，已探测 clusters 完整覆盖全局 GT top10，因此 exact rerank 后可恢复真实 top10”这一 `exact-top10-match-rate` 指标上，`faiss_ivfrq` 与 `ivf_rabitq_rerank` 的最小阈值都接近全量探测，分别约为 `2029` 和 `2032`。这说明该指标本身很苛刻，且两种方法差异不大，不能用来解释此前 `recall@10` operating point 的 probe gap。
- 当前结论：
  这个 change 已经足以支持下一步决策，即优先继续沿 stronger single coarse builder 路线推进，而不是回到 top-2 overlap / RAIR 或把 `exact-top10-match-rate` 当作 primary optimization target。

## Capabilities

### New Capabilities
- `single-coarse-builder-parity`: 在 single-assignment 语义下，支持按阶段比较分层 SuperKMeans、单层 SuperKMeans 与 Faiss IVF coarse clustering，并给出现有系统的 builder parity 诊断结论。
- `faiss-single-coarse-builder`: 复用 Faiss IVF coarse quantizer 训练 single-assignment centroids 与 primary assignments，并导出与现有 IVF build / serving 流程兼容的 coarse partition 工件。

### Modified Capabilities
- `benchmark-infra`: 扩展 benchmark / summary / tracker 输出，使 builder identity、阶段 gate 状态以及 target-recall probe threshold 能被统一记录和比较。

## Impact

- 影响代码：`IvfBuilder` 配置、single coarse builder selector、Faiss clustering export / import 路径、formal-study 评估脚本与 summary 聚合逻辑。
- 影响系统：single-mode coarse partition 训练流程、builder 元数据、diagnostic benchmark 输出，以及用现有 serving path 比较不同 coarse builder 的实验流程。
- 依赖关系：复用 `coarse-builder-and-cover-diagnostics` 已有 builder 对照链路、`ivf-rabitq-rerank-baseline` 中的 Faiss coarse clustering 参考实现，以及现有 e2e / vector-search benchmark 输出树。
