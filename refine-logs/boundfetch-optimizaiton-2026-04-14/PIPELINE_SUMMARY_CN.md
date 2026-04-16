# 管道总结

**问题**：在主曲线刷新后，BoundFetch 已进入 `0.97-0.99` 高召回区间，当前剩余问题是查询 CPU 热路径而不是 baseline 缺失。
**最终方法论文**：当前工作流只聚焦 BoundFetch 方法继续优化，目标是在固定 baseline 结果输入下进一步压缩 query-prep / probe 成本。
**日期**：2026-04-16

## 当前分工
- 本任务：方法继续优化
- 并行任务：所有 baseline 实验与图表维护

## 贡献快照
- 主要贡献：在现有 preload + corrected `bits=4` 路径上继续优化 query-prep / probe。
- 可选辅助贡献：无。
- 明确非贡献：
  - 声称纯 ANN 优越于调优后的图索引
  - 声称 FlatStor 是唯一有意义的存储后端
  - 声称每个搜索族必须与每个存储后端配对

## 必须证明的声明
- C1：当前下一步优化应直接落在 query-prep / probe CPU 热路径上。
- C2：`QuantizeQuery14Bit`、`PrepareQueryRotatedInto` 与 probe 本身是当前最主要的优化对象。

## 论文边界
- 主论文必须证明：
  - 暖稳态 E2E 服务仍然是正确的评估目标
  - 下一步有意义的 BoundFetch 优化是聚类侧预加载，而非更多盲目的 `io_uring` 微调
  - BoundFetch 仍明显强于 IVF 家族基线
  - DiskANN 仍是强上界参考，不应被移除
- 附录可支撑：
  - 额外的存储后端变体
  - 残余队列深度或模式敏感性检查
  - 在 Deep1M / Deep8M 上的通用性
- 有意裁剪的实验：
  - 冷启动 / drop-cache 协议
  - 完整的 `search x storage` 笛卡尔基准测试矩阵
  - 在预加载结果明确前又一轮提交路径调优

## baseline 输入约定

- baseline 结果继续保留为外部输入。
- 本任务不再负责继续跑 baseline sweep。
- 优化后只需要在相同报告口径下复测 BoundFetch。

## 当前解读

- BoundFetch 当前最高召回点：`epsilon=0.90, nprobe=512, alpha=0.01`，`0.9887 / 1.5114ms / 2.2997ms`
- BoundFetch 当前实用高召回点：`epsilon=0.90, nprobe=512, alpha=0.05`，`0.9694 / 1.4785ms / 2.3044ms`
- 当前热点：`QuantizeQuery14Bit`、`PrepareQueryRotatedInto`、probe
- 当前实际结论：
  - preload 线已经完成
  - 本任务现在应优先做有限轮次 CPU 热路径优化
  - baseline 继续扩展由并行任务维护

## 决策约束
- 不要从论文中移除 DiskANN，除非确实无法进行可复现的集成且有明确文档说明。
- 在分层计划完成之前，不要运行完整的 `search x storage` 矩阵。
- 在 `R042-R056` 完成之前，不要开始又一轮提交路径微优化。
- 如果 `R042-R056` 未能创建有用的新 BoundFetch 前沿区域，冻结低级优化并将工作重心转向分层权衡故事。

## 下一步
- 进入 R068：每查询预处理优化
