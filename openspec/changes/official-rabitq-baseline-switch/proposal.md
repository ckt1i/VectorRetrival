## Why

当前 formal-study 的 `IVF-RQ` baseline 仍然锚定在 Faiss `IVFResidualQuantizer` 路径上，但最近的排查表明，它在 COCO 100K、`nprobe=64` 时的 recall 明显低于与我们方法公平比较所需的工作区间。仓库里已经有一条基于官方 `RaBitQ-Library` 的 `ivf_rabitq_rerank.py` 路径，因此 baseline 口径应该切换到该实现，并在统一的端到端计时范围下重新标准化。

## What Changes

- 将当前 formal-study 主用的 `IVF-RQ` baseline 入口从 Faiss `faiss_ivfrq.py` 路径切换到官方 `ivf_rabitq_rerank.py` 路径。
- 统一新的 baseline 口径，使压缩 / 量化搜索保持常驻内存，而精确 rerank 从磁盘读取原始向量。
- 将计时范围与当前比较口径对齐：测得的 query latency MUST 包含压缩候选搜索、原始向量 rerank，以及 payload / 原始数据 fetch。
- 保留严格的 overlap `recall@10` 语义，并确保重跑后的工作点在 COCO 100K 的 `nprobe=64` 下朝着 `~0.9 recall@10` 调优。
- 重新文档化 baseline 命名，使 Faiss `IVFResidualQuantizer` 仅作为参考路径，而不是主比较里 canonical 的 `IVF+RQ+rerank` baseline。

## Capabilities

### New Capabilities
- `official-rabitq-baseline`: 定义 canonical 的 formal-study `IVF+RaBitQ+rerank` baseline 口径，使用 `RaBitQ-Library`，包括常驻内存的量化搜索、磁盘原始向量 rerank，以及包含 payload 的计时。

### Modified Capabilities
- `benchmark-infra`: baseline benchmark 的注册、命名和报告口径发生变化，使 canonical 的 `IVF-RQ` 比较路径指向官方 `ivf_rabitq_rerank.py` 实现，并导出与 payload 对齐的延迟。

## Impact

- 受影响代码：`baselines/formal-study/scripts/run_vector_search/`、`baselines/formal-study/scripts/run_e2e_coupled/`、`third_party/ivf-rabitq-rerank-baseline/`、结果导出 / 报告脚本，以及实验文档。
- 受影响系统：formal-study baseline 执行、benchmark 结果解释，以及论文表格中的对比项。
- 依赖项：本地已有的 `RaBitQ-Library` checkout、当前的 `ivf-rabitq-rerank-baseline` C++ core，以及 formal-study 使用的 payload backend 集成。
