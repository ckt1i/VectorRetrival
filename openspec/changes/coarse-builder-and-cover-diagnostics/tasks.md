## 1. Coarse Builder 接入

- [ ] 1.1 为 coarse IVF 构建路径引入显式 `coarse_builder` 配置，并为现有 SuperKMeans 路径记录 builder identity
- [ ] 1.2 接入 `faiss_kmeans` coarse builder，确保其能导出与现有诊断流程兼容的 centroids 和 primary assignments
- [ ] 1.3 在构建元数据与输出目录中写入 builder identity、metric、nlist 与 assignment mode

## 2. Pure Coarse-Cover 诊断

- [ ] 2.1 实现 pure coarse-cover 诊断模式，只测已探测 coarse clusters 对 GT top-k 的覆盖，不依赖 CRC、epsilon 或 cluster 内 pruning
- [ ] 2.2 为 `single`、`redundant_top2_naive` 和 `redundant_top2_rair` 三种 assignment mode 输出统一的 coarse-cover 结果
- [ ] 2.3 支持固定 probe schedule 的 cover-vs-probe 导出，至少覆盖 high-recall 区间所需的 probe 范围

## 3. 汇总输出与阈值结果

- [ ] 3.1 生成 coarse-cover 原始结果表，包含 dataset、builder、assignment mode、nprobe、topk 和 coarse cover
- [ ] 3.2 生成派生阈值结果，至少报告达到 `0.95`、`0.99`、`0.995` coarse cover 的最小 `nprobe`
- [ ] 3.3 扩展 benchmark summary / aggregation 流程，使 coarse-cover 诊断结果可直接聚合比较

## 4. 验证与实验回填

- [ ] 4.1 在 `coco_100k` 上完成 `1024` 与 `2048` 两档 `nlist` 的 coarse-builder 对照运行
- [ ] 4.2 在相同 builder 下完成 `single`、`redundant_top2_naive`、`redundant_top2_rair` 的 coarse-cover 对照
- [ ] 4.3 将诊断结论写回实验跟踪文档，明确 builder gap、assignment gap 与下一步算法修改方向
