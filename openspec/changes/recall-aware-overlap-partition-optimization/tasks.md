## 1. Recall-Aware 信号准备

- [ ] 1.1 定义 recall-aware secondary assignment 所需的 build-time signal，并明确最小可行输入来源（local neighbors、calibration queries 或其近似）
- [ ] 1.2 在 `IvfBuilder` 配置与元数据中加入 recall-aware policy identity 与必要参数字段
- [ ] 1.3 为 recall-aware policy 建立与现有 `single`、`redundant_top2_naive`、`redundant_top2_rair` 并行可比较的配置路径

## 2. Secondary Assignment 实现

- [ ] 2.1 实现 recall-aware 的 complementary second-cluster 选择逻辑，而不是仅依赖 second-nearest 或 residual-aware 评分
- [ ] 2.2 保留现有 secondary assignment 路径，作为稳定回退基线
- [ ] 2.3 输出 assignment policy 元数据，确保后续 coarse-cover 结果能区分不同 overlap 策略

## 3. Overlap-Aware Centroid Refinement

- [ ] 3.1 设计并实现 overlap membership 下的 centroid refinement stage
- [ ] 3.2 支持显式开关与 refinement 轮次配置，允许对照“有 refinement / 无 refinement”
- [ ] 3.3 记录 refinement stage 元数据与必要 cluster statistics，便于对照 refined 与 non-refined 分区

## 4. 验证与评估

- [ ] 4.1 使用 coarse-cover diagnostics 对比 residual-aware 与 recall-aware secondary assignment 的 probe budget 改善
- [ ] 4.2 验证 overlap-aware centroid refinement 是否进一步降低达到 `0.99` coarse cover 所需的 `nprobe`
- [ ] 4.3 将实验结论回填到实验计划与分析文档，明确该方法是否进入下一轮 serving 对比
