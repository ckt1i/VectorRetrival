## Context

仓库里目前在 formal-study 中暴露了两条不同的 `IVF+RQ` 风格 baseline 路径：

- `faiss_ivfrq.py`: Faiss `IndexIVFResidualQuantizer` / `IndexIVFLocalSearchQuantizer`
- `ivf_rabitq_rerank.py`: official `RaBitQ-Library` IVF compressed search plus external exact rerank

最近的排查显示，Faiss RQ 路径并不是 COCO 100K 上 intended 的 `IVF+RQ+rerank` 比较口径的稳定替代。它还会制造报告歧义，因为主比较文案里写的是 `IVF-RQ baseline`，但实现上可能指向 Faiss RQ，也可能指向官方 RaBitQ-Library IVF。与此同时，公平比较要求当前计时口径把三个阶段对齐：

1. compressed / quantized candidate search
2. exact rerank by reading raw vectors from disk
3. payload / original-data fetch after final top-k is produced

因此，这个设计需要把官方 `ivf_rabitq_rerank.py` 路径提升为 canonical 比较路径，同时保留 benchmark-infra 的报告能力、严格 overlap recall，以及包含 payload 的计时。

## Goals / Non-Goals

**Goals:**
- 让 `ivf_rabitq_rerank.py` 成为 canonical 的 formal-study `IVF+RQ+rerank` baseline 入口。
- 通过官方 `RaBitQ-Library` IVF index 保持量化搜索常驻内存。
- 要求 exact rerank 从磁盘读取原始向量，而不是依赖预加载的 raw-vector 内存。
- 要求 benchmark 延迟在一个对齐的计时范围内包含 compressed search、磁盘 rerank 和 payload fetch。
- 保留严格的 overlap `recall@10` 语义，并支持在 COCO 100K、`nprobe=64` 上把工作点重新调到接近 `0.9 recall@10`。
- 保留 Faiss RQ 作为诊断 / 参考路径，而不是 canonical 的主表 baseline。

**Non-Goals:**
- 不把内部 VectorRetrival serving engine 替换成 `RaBitQ-Library`。
- 不重新设计主产品流水线里的 CRC / SafeOut / overlap scheduling 逻辑。
- 不保证每个数据集都能在 `nprobe=64` 下达到 `0.9 recall@10`；当前直接目标是 COCO 100K 的比较设置。
- 不把 formal-study 的 recall 定义改成别的语义，仍然保持 overlap recall。

## Decisions

### Decision: Canonical `IVF-RQ` baseline identity switches to `ivf_rabitq_rerank.py`
主比较口径 SHALL 使用现有的 `third_party/ivf-rabitq-rerank-baseline` C++ core，并由 `baselines/formal-study/scripts/run_vector_search/ivf_rabitq_rerank.py` 封装。

Rationale:
- 这条路径已经把官方 `RaBitQ-Library` IVF 作为 compressed-search kernel。
- 相比 Faiss `IVFResidualQuantizer`，它更符合 intended 的算法族。
- 这避免了强行深改 `faiss_ivfrq.py`，因为它当前的抽象强依赖 Faiss。

Alternatives considered:
- Rewire `faiss_ivfrq.py` to invoke `RaBitQ-Library` under the same filename.
  Rejected because it would preserve a misleading identity while replacing the entire backend model.
- Keep Faiss RQ as the main baseline and only add RabitQ-Library as a side result.
  Rejected because it does not solve the current fairness and naming problem.

### Decision: Quantized search stays memory-resident, exact rerank stays disk-backed
官方 baseline SHALL 继续把压缩后的 `official.ivf_rabitq.index` 加载到内存中进行查询，但 exact rerank SHALL 在查询执行期间从磁盘上的 raw-vector 文件（`vectors.bin` 或等价文件）读取原始向量。

Rationale:
- 这样保留了想要的系统切分：压缩 ANN kernel 在内存中，exact rerank 依赖存储中的原始数据。
- 这与用户要求的比较模型一致，也避免了隐藏的 raw-vector 预加载优势。

Alternatives considered:
- Memory-map or fully preload raw vectors for rerank.
  Rejected because it changes the comparison contract and narrows the gap to a RAM-only benchmark.

### Decision: Payload fetch becomes a required timed stage for the canonical baseline
canonical baseline 的延迟范围 SHALL 包含：
- compressed candidate search 时间
- exact raw-vector rerank 时间
- final top-k 确定后的 payload / original-data fetch 时间

Rationale:
- 当前比较要求 baseline 计时必须与我们方法的端到端路径一致。
- 如果把 payload fetch 放在计时外，会低估 baseline 成本，也会破坏 apples-to-apples 的 latency 结论。

Alternatives considered:
- Keep payload fetch as an untimed post-process.
  Rejected because this was the earlier source of timing misalignment.

### Decision: Faiss RQ remains a reference path with explicit naming
Faiss 路径 SHALL 保持可运行，但 SHALL 不再被视为主表里的 `IVF-RQ baseline`。报告和文档 SHALL 明确把它命名为 `faiss_ivfrq` / `Faiss IVFResidualQuantizer reference`。

Rationale:
- 这样可以保留诊断价值，同时不把两个不同的 compressed-search kernel 混为一谈。
- 这能避免未来结果表把 Faiss-RQ 和官方 RaBitQ-Library 的工作点静默混在一起。

## Risks / Trade-offs

- [Risk] 官方 `RaBitQ-Library` 路径在 `nprobe=64` 下可能仍然需要明显重调才能接近 `0.9 recall@10`。  
  → Mitigation: 在 spec 里明确目标工作点，并把 `candidate_budget`、`nprobe` 和计时范围验证的 sweep / tuning 任务记录下来。

- [Risk] 给 `ivf_rabitq_rerank.py` 加上的 payload-inclusive timing 可能与现有历史指标不一致。  
  → Mitigation: 在结构化元数据里显式导出 timing scope，并把历史输出只保留为参考，不再当作 canonical 结果。

- [Risk] canonical baseline 重命名可能会让旧图表或脚本失效，因为它们默认 `faiss_ivfrq` 是主 `IVF-RQ` 结果。  
  → Mitigation: 更新 benchmark-infra 的报告规则，并在 change artifacts 中注明命名迁移。

- [Risk] C++ sidecar baseline 目前暴露了 search 和 rerank 计时，但还未必把 payload fetch 放进同一个 wall-clock 范围。  
  → Mitigation: 把 payload-inclusive timing 作为必做实现任务，而不是可选后续。

## Migration Plan

1. 保留现有 Faiss RQ 路径作为非 canonical 参考。
2. 升级 `ivf_rabitq_rerank.py` 及其 C++ core，使端到端计时包含 payload fetch。
3. 更新 formal-study 的命名、报告和选择逻辑，使 canonical 的 `IVF-RQ` 比较使用官方 RabitQ 路径。
4. 在 `nprobe=64` 下重新跑 COCO 100K sweep，并调好 candidate-budget / payload 对齐的计时。
5. 重新生成比较摘要，并确认新的 baseline 口径就是主表消费的口径。

Rollback strategy:
- 如果官方 RabitQ 路径不能快速稳定，就把改动先限制在报告 / 入口层，并临时把 canonical baseline 迁移标记为未完成，而不是静默回退到 Faiss RQ。

## Open Questions

- canonical reporting label 应该是 `ivf_rabitq_rerank`、`official_ivf_rabitq_rerank`，还是在论文表格里保留 `IVF-RQ+rerank baseline`，并把 provenance 单独存放？
- canonical 对比要求的默认 payload backend 是 `flatstor`、`lance`，还是别的 backend？
- 在 COCO 100K 上达到 `nprobe=64` 目标工作点之前，可接受的调优预算到底是多少？
