## Context

`formal-baseline-study-execution` 仍然是更大范围的 formal baseline 合同，但本科论文当前只需要 `coco_100k` 与 `msmarco_passage` 的最小主实验闭环。新的 thesis plan 还引入了一个比旧 formal spec 更严格的执行约束：主实验在进入 sanity 和 full sweep 之前，必须先由 `bench_e2e` 生成每个数据集的 canonical 聚类/索引工件，然后验证 BoundFetch 与 baseline 都能消费这些既有工件，而不是各自重新对原始向量重建 coarse clustering。

当前实现上有两个现实约束需要显式处理：

- `bench_e2e` 已支持 `--coarse-builder {superkmeans,hierarchical_superkmeans,faiss_kmeans}`，并支持 `--index-dir` 复用既有索引目录，因此它适合成为 canonical artifact producer。
- formal-study 可复用 runner 目前主要位于 `/home/zcq/VDB/baselines/formal-study/legacy/scripts/`。其中：
  - `faiss_ivfpq_refine.py` 仍通过内部 cache 自建或读取 Faiss index，没有显式 external index 输入。
  - `ivf_rabitq_rerank.py` 会在自己的 `index_cache/` 下建索引，但尚未把“消费外部 canonical artifact”作为正式 CLI 合同。
  - `ivf_rabitq_rerank` 的 coupled E2E 已支持 `--vector-run-dir`，说明“先 vector-search，再复用输出跑 E2E”的模式是现成可复用的。

因此，这个 change 的技术核心不是再发明一套实验矩阵，而是把 thesis 主实验改成“builder-first, artifact-reuse, sweep-second”的执行架构，并把这套架构纳入 formal-study 的 tracker/output 体系。

## Goals / Non-Goals

**Goals:**

- 为 `T000-T015` 定义单独、可执行的 thesis 主实验合同。
- 固化 `coco_100k` 与 `msmarco_passage` 的 canonical builder 策略：
  - `coco_100k` 使用 `faiss_kmeans`
  - `msmarco_passage` 使用 `hierarchical_superkmeans`
- 要求 BoundFetch-Guarded、`IVF+PQ`、`IVF+RaBitQ` 在 full sweep 前通过“使用既有索引文件或无重训导入工件”的验证 gate。
- 继续复用 formal-study 的输出、tracker、summary 口径，而不是再起一套 thesis-only schema。
- 将 `tasks.md` 拆细到可直接映射 `T000-T015` 内部子步骤的粒度。

**Non-Goals:**

- 不覆盖 `deep1m_synth` / `deep8m_synth`、appendix 数据集或 Amazon ESCI。
- 不覆盖 `topk=20/50`、triage ablation、scheduling ablation。
- 不把 DiskANN、IVFRQ、IVFPQR 纳入这版 thesis 主表合同。
- 不要求在这个 change 中一次性完成所有实验实现代码；本 change 只冻结后续实现和执行必须满足的合同。

## Decisions

### 1. `bench_e2e` 成为 canonical index artifact 的唯一生产者

`coco_100k` 与 `msmarco_passage` 的主实验不再允许每个方法独立决定 coarse clustering 和 index build 口径。每个数据集先由 `bench_e2e` 生成 canonical artifact，再由各方法复用。

选择这个方案的原因：

- `bench_e2e` 已经具备 coarse-builder 选择与 `--index-dir` 复用能力，最接近项目内现有的统一 builder 入口。
- thesis 需要比较的是方法和系统路径，而不是多套 builder 口径叠加后的混合差异。
- 先冻结 canonical artifact，后面出现 recall 或 tail latency 差异时，因果会更清晰。

备选方案：

- 让 BoundFetch、PQ、RaBitQ 各自重建索引：实现快，但 builder 差异会污染结果，不接受。
- 只冻结 centroid/assignment，不冻结 index：仍然会留下系统各自 build path 的不确定性，不接受。

### 2. “复用既有索引”按严格优先级定义为 direct-use，必要时允许无重训导入

对 thesis 主实验，理想路径是各方法直接使用 canonical index 文件。若某方法的 runtime 不能直接打开 canonical 文件格式，则允许一次性导入，但导入过程只能消费 canonical artifact，不能重新读取 raw vectors 进行训练、聚类或重分配。

选择这个定义的原因：

- 这和用户要求的“先建索引，再验证方法可直接用现有索引文件跑实验”一致。
- 当前 `faiss_ivfpq_refine` 与 `ivf_rabitq_rerank` 的 runner 现实上并不都具备 direct-use CLI，因此需要给实现留一个无重训 adapter 余地。
- 真正需要避免的是“每个系统重新对原始数据训练/聚类”，而不是苛求所有系统二进制格式完全一致。

备选方案：

- 强制所有系统二进制级 direct-open 同一个 index 文件：语义最干净，但对 `ivf_rabitq_rerank` 风险过高。
- 允许 fallback 到 raw-vector rebuild：会破坏 thesis 的 canonical-artifact 约束，不接受。

### 3. 复用 formal-study tracker/output contract，但新增 index-build 与 reuse-validation 事件

这次实验仍然写入 `/home/zcq/VDB/baselines/formal-study/` 下的 manifests、trackers 与 outputs，但 tracker 需要能显式表达：

- asset gate
- canonical index build
- index reuse validation
- sanity run
- full sweep
- dataset summary

选择这个方案的原因：

- 现有 thesis plan 已经引用 formal-study controls，继续复用同一套输出根更利于后续汇总。
- 新加事件类型比另起一套 thesis-only 输出树更便于回收旧工具链。

备选方案：

- thesis 单独维护全新 tracker/output schema：隔离更强，但会重复已有 formal-study 结构，不接受。

### 4. `tasks.md` 按 gate 和数据集切片细分，不再只列粗 milestone

任务将拆成：

- workspace / runner restoration
- `T000-T001` asset checks
- `coco_100k` canonical index build + reuse validation
- `coco_100k` sanity + full sweep
- `msmarco_passage` canonical index build + reuse validation
- `msmarco_passage` sanity + full sweep
- aggregation / tracker closure

选择这个方案的原因：

- 用户明确要求 task 内部继续细分。
- 真正的实验阻塞点是 gate，而不是最终图表文件。

## Risks / Trade-offs

- [Active runner 仍在 `legacy/scripts/`] → Mitigation: 在实现阶段先恢复当前执行入口，至少要让 README、manifests 和实际 runner 路径一致。
- [`faiss_kmeans` 与 `hierarchical_superkmeans` 产生的 artifact 格式未必能被全部 baseline 直接打开] → Mitigation: 允许无重训导入，但必须记录 provenance，并禁止重新对 raw vectors 训练。
- [MS MARCO 的 qrels 可能延迟] → Mitigation: `T010-T015` 允许先以 `recall@10` 为主完成 first pass，同时将 `MRR@10/nDCG@10` 标记为 delayed 而不是阻塞 full sweep。
- [Lance 运行链路可能晚于 FlatStor 稳定] → Mitigation: 规格中把 Lance 保持为 SHOULD-level 主实验分支；若 blocked，必须显式记录，不得静默跳过。
- [清理 formal-study 后，README 对脚本位置的描述与实际磁盘状态不完全一致] → Mitigation: 把这视为实现阶段的 workspace repair 子任务，而不是在 proposal 中假装它已经正确。

## Migration Plan

1. 重建 `thesis-minimal-main-sweep` 的 OpenSpec artifacts，作为新的 thesis 主实验合同。
2. 恢复或重新绑定 formal-study 当前 runner 入口，使活动 workspace 能调用现有 legacy runner。
3. 为 `bench_e2e` 增加或整理 canonical index build 输出规范。
4. 为 BoundFetch、PQ、RaBitQ 的 runner 增加 external artifact consumption CLI，并实现 reuse validation gate。
5. 在 `RUN_STATUS.csv` 与 `FAILURES.md` 中加入 asset/index/sanity/full-sweep 的细粒度记录。
6. 依序执行 `T000-T015`，先 `coco_100k`，再 `msmarco_passage`。

## Open Questions

- `faiss_ivfpq_refine` 最终是直接读取 `bench_e2e` 导出的 Faiss index，还是通过一层 formal-study wrapper 适配为 shared cache？
- `ivf_rabitq_rerank` 的 canonical import 最小可接受工件集合是什么：完整 index 目录、centroids+assignments，还是额外的 metadata manifest？
- thesis 主实验的 BoundFetch canonical artifact 是否要求同时包含 preload-ready payload metadata，还是只冻结向量索引与聚类工件？
