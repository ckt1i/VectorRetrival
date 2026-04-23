## 1. 先决条件复核

- [x] 1.1 用 single-builder parity 对照确认当前 high-recall gap 是否首先来自 coarse builder，而不是 overlap policy 本身
- [x] 1.2 记录结论：`superkmeans` 虽优于 `hierarchical_superkmeans`，但仍未达到 baseline operating point；`faiss_kmeans` 已把 `recall@10=0.99` 的最小 `nprobe` 压到 baseline 量级
- [x] 1.3 记录附加结论：`exact-top10-match-rate` 在 `faiss_ivfrq` 与 `ivf_rabitq_rerank` 上都接近全量 probe，不能作为解释 recall probe gap 的主指标

## 2. 暂停 Recall-Aware Overlap 主线

- [x] 2.1 将 recall-aware secondary assignment、top-2 overlap、RAIR 和 overlap-aware centroid refinement 从当前主执行路径中降级为后置候选，而不是当前优先实现项
- [x] 2.2 保留现有 `single`、`redundant_top2_naive`、`redundant_top2_rair` 路径作为历史对照基线，但不再把它们视为缩小 single-mode probe gap 的主攻方向
- [x] 2.3 明确 gate：只有当 stronger single coarse builder 仍无法达到 `IVF+RQ` / `IVF+RaBitQ` baseline 的 probe 水平时，才重新启用 recall-aware overlap 任务

## 3. 后续工作迁移

- [ ] 3.1 将 single-builder parity 的结论迁移为新的主 change 输入，优先推进 single coarse builder 默认化或 builder-family 替换
- [ ] 3.2 若仍保留本 change，则将其范围改写为“仅在 stronger single builder 已稳定后，再评估 overlap 是否带来额外边际收益”
- [ ] 3.3 为未来可能重启的 overlap 路线保留最小实验契约：必须在 `faiss_kmeans` 或同等级 primary builder 之上验证边际收益，而不是再与弱 primary builder 绑定

## 4. 文档与决策回填

- [ ] 4.1 将本 change 的 proposal / design 更新为“deferred change”口径，明确其不再是当前实现主线
- [ ] 4.2 在实验计划与分析文档中回填当前决策：single-builder parity 已经比 overlap-aware 路线更直接地解释并缩小 recall probe gap
- [ ] 4.3 明确 archive / rename / supersede 方案，避免后续继续按旧任务列表推进不再成立的 overlap 实现
