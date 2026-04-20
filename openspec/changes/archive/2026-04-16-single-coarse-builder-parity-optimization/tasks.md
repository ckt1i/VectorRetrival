## 1. Builder Selector And Metadata

- [x] 1.1 为 single coarse build 路径增加显式 builder selector，至少支持 `hierarchical_superkmeans`、`superkmeans`、`faiss_kmeans`
- [x] 1.2 为 single partition 输出补充 builder identity、phase gate 状态与 target-threshold summary 元数据字段
- [ ] 1.3 确保现有 single-mode build / serving 流程在未显式切换 builder 时仍保持当前默认行为

## 2. Phase 1: Single-Layer SuperKMeans Parity

- [x] 2.1 接入单层 `superkmeans` single-assignment coarse build 路径，并导出与现有流程兼容的 centroids / assignments
- [x] 2.2 在 `coco_100k` 上对照 `hierarchical_superkmeans` 与 `superkmeans`，保持相同 `nlist`、metric、single assignment 与评估契约
- [x] 2.3 扩展 summary 输出，记录 phase-1 gate 结论以及达到目标 recall / candidate-recall 所需的最小 `nprobe`
  Phase-1 result: `coco_100k / nlist=2048 / topk=10 / target recall@10=0.99` 下，`hierarchical_superkmeans` 在 `nprobe<=384` 内未达到目标，最佳仅 `0.9861@384`；单层 `superkmeans` 在 `nprobe=384` 刚好达到 `0.9900`。说明“去掉分层”能带来改善，但仍未接近 baseline 的 probe 水平，phase-1 gate 判定为 fail，需要进入 phase 2。

## 3. Phase 2: Faiss Single Builder Parity

- [x] 3.1 复用或对齐 baseline 的 Faiss IVF coarse clustering 逻辑，导出 single centroids 与 nearest-centroid assignments
- [x] 3.2 将 `faiss_kmeans` 导出的 coarse partition 接入现有系统的 single-mode build / serving 对照流程，而不替换现有 query / candidate / rerank 路径
- [x] 3.3 在 phase-1 失败条件下执行 `faiss_kmeans` parity 运行，并记录其是否达到 `IVF+RQ` / `IVF+RaBitQ` baseline 的 probe 水平
  Phase-2 result: 复用 `faiss_kmeans` 后，当前方法在 `coco_100k / nlist=2048 / topk=10` 下达到 `recall@10=0.99` 的最小 `nprobe` 可压到 `256`，已经对齐此前 `IVF+RQ` / `IVF+RaBitQ` baseline 所在量级，说明 single-mode high-recall gap 的主因在 coarse builder，而不是 query/rerank 主路径。

## 4. Threshold Evaluation And Decision

- [x] 4.1 为 builder parity 运行统一输出达到目标 `recall@10` 或 candidate-recall 指标所需的最小 `nprobe`
- [x] 4.2 将 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的结果聚合到同一 summary 中，支持按 builder 和阶段 gate 比较
- [x] 4.3 回填实验结论，明确下一步应停留在 stronger single builder，还是需要另起 change 引入更复杂的 retrieval-aware refinement
  Aggregated decision:
  `single_builder_recall_probe_coco_100k_nlist2048_summary/summary.json` 已形成 phase-1 summary；
  `exact_top10_match_coco_100k_nlist2048/summary.json` 显示“在 99% 概率下，probed clusters 已完整覆盖全局 GT top10”这一更纯粹指标上，`faiss_ivfrq` 与 `ivf_rabitq_rerank` 的门槛都接近全量探测，分别约为 `2029` 与 `2032`，差异很小。
  因此当前 change 的明确结论是：需要优先继续沿 stronger single coarse builder 路线收敛 `recall@10` operating point；`exact-top10-match-rate` 不能解释此前 `recall@10` probe gap，后续不应把它当作 primary optimization target。
