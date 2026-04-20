## Context

当前仓库的 single-mode coarse partition 默认仍以 `hierarchical_superkmeans` 为主路径，但现有 coarse-cover、exact-top10-match 与端到端 recall 结果已经表明：high-recall probe gap 的主矛盾应先回到 primary coarse builder 本身，而不是继续优先投入 top-2 overlap / RAIR。与此同时，`IVF+RQ` 与 `IVF+RaBitQ` baseline 使用的是更标准的 Faiss IVF coarse quantizer 训练与 single nearest-centroid assignment 路径；在继续设计更复杂的 retrieval-aware refinement 之前，最小变量原则要求先回答两个问题：

1. 当前 gap 是否主要来自 `hierarchical_superkmeans` 这条“分层” builder 路径，而不是 SuperKMeans family 本身？
2. 如果 single-layer SuperKMeans 仍不足以缩小 gap，那么仅通过复用 Faiss-style single coarse clustering，是否已经可以把 single-mode probe budget 拉到 baseline 水平？

本 change 因此采用 staged parity design，而不是直接引入新的 recall-aware partition objective。第一阶段只验证“分层 vs 单层”是否决定了当前差距；第二阶段只在第一阶段失败时启用，用 Faiss IVF clustering 作为 single coarse builder parity 版本接入现有系统。整个设计刻意保持 serving contract 不变，使剩余差距可被清晰归因到 coarse partition 之外的因素。

## Goals / Non-Goals

**Goals:**
- 在 single-assignment 语义下，按阶段比较 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的 coarse partition 效果。
- 第一阶段明确验证：仅把分层 SuperKMeans 改为单层 SuperKMeans 时，达到 high-recall target 所需的 `nprobe` 是否显著下降。
- 若单层 SuperKMeans 仍不足以达到 baseline probe 水平，第二阶段接入 Faiss IVF coarse clustering，并把导出的 centroids / assignments 接入现有系统的 single-mode build 与 serving 流程。
- 用统一的 builder-parity 评估契约比较各阶段结果，直接输出目标 `recall@10` 或等价 candidate-recall operating point 所需的最小 `nprobe`。
- 为下一轮是否需要更复杂的 retrieval-aware refinement 提供 clean ablation：若 Faiss-style single builder 已足够，则先不引入更重的 partition algorithm。

**Non-Goals:**
- 本 change 不引入 top-2 overlap、RAIR、secondary assignment 或 overlap-aware centroid refinement。
- 本 change 不重写 serving-time query pipeline、candidate generation 逻辑或 rerank 路径。
- 本 change 不要求在第一阶段就发明新的 query-aware / calibration-aware clustering objective。
- 本 change 不修改 `.clu` / `.data` 格式，除非为了记录 builder identity 与阶段 gate 元数据必须追加兼容元数据。

## Decisions

### 1. 采用两阶段 gate，而不是直接替换为 Faiss builder
设计 SHALL 先执行 single-layer SuperKMeans 对照，只有在其结果仍不能显著缩小 high-recall probe gap 时，才启用 Faiss IVF clustering parity 路径。

原因：
- 这能先隔离“分层构建误差”是否是 current builder 的主因。
- 如果单层 SuperKMeans 已显著改善，则无需过早依赖 Faiss builder 作为默认实现。

备选方案：
- 直接跳到 Faiss builder：否决，因为会失去“分层 vs 单层”的定位证据。
- 同时并行推进单层 SuperKMeans 与 recall-aware clustering：否决，因为变量过多，难以归因。

### 2. Faiss parity 版本只复用 coarse clustering，不复用 baseline 的整个检索链路
第二阶段 SHALL 仅复用 Faiss IVF coarse quantizer 训练与 single nearest-centroid assignment 语义，并将其导出的 centroids / assignments 接入现有系统的 single-mode partition build 与 serving 对照流程。

原因：
- 目标是隔离 coarse partition 变量，而不是把 baseline 整条 `RQ + search + rerank` 路线整体移植进来。
- 这允许直接测量 “our system + faiss-style single partition” 与 “baseline system” 之间剩余差距。

备选方案：
- 直接调用 baseline 全套搜索路径：否决，因为会把 coarse builder 和 downstream pipeline 混在一起。
- 完全重写新的 Faiss-like builder 而不参考 baseline：暂缓，第一版优先复用已验证的参考实现逻辑。

### 3. 成败判据以 target-recall probe threshold 为主，而不是只看 coarse cover
builder parity 评估 SHALL 直接报告达到目标 `recall@10` 或等价 candidate-recall operating point 所需的最小 `nprobe`，并以 `IVF+RQ` / `IVF+RaBitQ` baseline 已观测的 probe 水平作为主要参考标准。

原因：
- 现有诊断已经显示 exact-top10-match 与最终 recall 有明显错位；继续只看 coarse cover 会过度强调过严指标。
- 当前 change 的产品问题是 probe budget 是否能逼近 baseline，而不是单独提升某个中间几何指标。

备选方案：
- 继续以 coarse-cover 作为唯一主指标：否决，因为它不足以决定 single builder parity 是否已经足够。
- 仅以最终 recall 作为指标：否决，因为仍需保留 candidate / builder 中间证据帮助定位 remaining gap。

### 4. Builder identity 与阶段 gate 作为显式元数据输出
所有 single builder parity 运行 SHALL 输出 builder identity、阶段 gate 状态（phase1_superkmeans_flat / phase2_faiss_parity）以及对应的 target-threshold summary。

原因：
- 本 change 的关键不只是“哪个结果更好”，而是“是在哪个 gate 阶段收敛或失败”。
- 没有显式阶段标签，后续难以判定是否该进入 retrieval-aware refinement 的下一轮提案。

备选方案：
- 只在文件名里暗含阶段：否决，因为会降低聚合与复现性。

## Risks / Trade-offs

- **[Risk] 单层 SuperKMeans 相比当前分层路径只带来轻微改善，无法回答是否应彻底迁移 builder family** → Mitigation: 预先定义 phase-1 fail 条件，并在 fail 后自动进入 Faiss parity gate。
- **[Risk] Faiss parity 版本显著改善 coarse partition，但现有 serving path 仍无法达到 baseline recall probe 水平** → Mitigation: 保留 “our system + faiss-style single partition” 的独立结果，用于隔离剩余差距属于 downstream pipeline。
- **[Risk] 复用 Faiss clustering 参考实现会增加一条新的 builder 依赖路径** → Mitigation: 将其保持为显式 coarse builder selector，不替换当前 builder 作为无条件默认。
- **[Risk] target-recall threshold 受 candidate budget / rerank 设置影响，导致 builder 比较仍带有 pipeline 噪声** → Mitigation: 同时导出 target recall threshold 与 candidate-recall threshold，确保 coarse builder 改善能被中间指标佐证。

## Migration Plan

1. 为 single builder 路径增加显式 selector，区分 `hierarchical_superkmeans`、`superkmeans`、`faiss_kmeans`。
2. 完成 phase 1：单层 SuperKMeans build / diagnostic / serving 对照，并与当前分层路径比较 probe threshold。
3. 若 phase 1 未达到预设成功标准，则启用 phase 2：复用 Faiss IVF clustering 导出 single centroids / assignments，并接入现有 single-mode build 流程。
4. 在统一 summary 中记录各 builder 的 target threshold、candidate threshold、阶段 gate 状态与结论。
5. 根据结果决定：
   - 若 Faiss parity 已足够逼近 baseline，则优先收敛到 stronger single builder；
   - 若 Faiss parity 仍不足，则下一轮 proposal 再引入 retrieval-aware refinement。

回滚策略：
- 保留现有 `hierarchical_superkmeans` 路径作为稳定默认回退。
- 将 `superkmeans` 与 `faiss_kmeans` builder 均置于显式配置开关之后，不影响现有 serving contract。

## Open Questions

- “显著提升” 的 phase-1 通过标准具体应该设为多少：相对 probe threshold 降幅，还是直接达到某个 baseline target？
- phase-2 的 Faiss parity 是否只要求在 `coco_100k` 达标，还是第一版就要求支持 `1024` / `2048` 两档 `nlist`？
- 为了保证对照公平，Faiss parity 版本是否需要固定与 baseline 相同的 train size、metric normalization 与 centroid export 格式？
