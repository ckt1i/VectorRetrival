# BoundFetch 研究精化与实验规划 — 中文文档索引

**项目名称**：BoundFetch
**完成日期**：2026-04-16
**状态**：方法继续优化进行中；baseline 任务已拆分为并行主线

---

## 📋 文档导航

### 核心方法文档

1. **[FINAL_PROPOSAL_CN.md](FINAL_PROPOSAL_CN.md)** — 最终提案
   - 问题陈述与现有方案的困境
   - 方法论文与三类 I/O 调度机制
   - 主要贡献与支撑贡献
   - 6 个关键声称
   - 消融实验清单
   - 论文定位与摘要

2. **[REFINEMENT_REPORT_CN.md](REFINEMENT_REPORT_CN.md)** — 精化报告
   - 从三个独立创新点到统一洞察的精化过程
   - 贡献层级的建立
   - 问题陈述的逐步聚焦
   - 舍弃的复杂性说明

3. **[REVIEW_SUMMARY_CN.md](REVIEW_SUMMARY_CN.md)** — 评审总结
   - 方法强度分析
   - 已解决的设计缺陷
   - 实现成熟度评估
   - 预期评审者疑虑及回应

---

### 实验规划文档

4. **[EXPERIMENT_PLAN_CN.md](EXPERIMENT_PLAN_CN.md)** — 方法优化实验计划
   - 6 个要验证的声称与对应实验
   - 硬件与环境要求
   - 8 个数据集说明
   - 9 个实验的完整描述（E1-E9）
   - 决策闸门和下一步规则
   - 4 阶段运行顺序
   - 计算预算估计

5. **[EXPERIMENT_TRACKER_CN.md](EXPERIMENT_TRACKER_CN.md)** — 方法优化追踪表
   - 完整的检查清单格式
   - 4 个阶段的具体任务分解
   - 论文图表清单

6. **[PIPELINE_SUMMARY_CN.md](PIPELINE_SUMMARY_CN.md)** — 方法优化一页总结
   - 一页纸总结问题、方法、贡献、声称
   - 6 个必须证明的声称列表
   - 首先启动的 3 个实验
   - 主要风险与缓解策略
   - 下一步行动

---

## 🎯 快速理解指南

### 如果你有 5 分钟
→ 阅读 [PIPELINE_SUMMARY_CN.md](PIPELINE_SUMMARY_CN.md) 的前 3 个部分

### 如果你有 20 分钟
→ 完整阅读 [PIPELINE_SUMMARY_CN.md](PIPELINE_SUMMARY_CN.md) + [FINAL_PROPOSAL_CN.md](FINAL_PROPOSAL_CN.md) 的前 4 个章节

### 如果你有 1 小时
→ 完整阅读所有 3 个核心文档（FINAL_PROPOSAL + REFINEMENT_REPORT + REVIEW_SUMMARY）

### 如果你要开始做实验
→ 从 [EXPERIMENT_PLAN_CN.md](EXPERIMENT_PLAN_CN.md) 的 CPU 优化部分开始，使用 [EXPERIMENT_TRACKER_CN.md](EXPERIMENT_TRACKER_CN.md) 跟踪进度

## 当前分工说明

- 方法优化主线：由当前 refine 文档维护
- baseline 主线：由并行任务维护，参考 [BASELINE_PLAN_CN.md](BASELINE_PLAN_CN.md)

---

## 🔑 关键概念快速参考

### 三类 I/O 调度
```
SafeIn    → 候选肯定在 Top-K    → 立即读完整记录（向量+原始数据）
Uncertain → 无法判断           → 只读向量，计算精确距离，再决定
SafeOut   → 候选肯定不在 Top-K → 跳过（零 I/O）
```

### 为什么安全？
RaBitQ 的误差界 `epsilon = c * 2^(-bits/2) / sqrt(dim)` 有理论保证，因此：
- SafeIn 和 SafeOut 不会错过真正的 Top-K（有可证明的 margin）
- Uncertain 候选通过精确距离计算消解

### 三个支撑部分
1. **ClusterStore (.clu)**：量化代码 + 地址列，利于批量扫描
2. **DataFile (.dat)**：向量与原始数据共存，利于单次读取完整记录
3. **io_uring 重叠调度**：并行化 CPU 探测、I/O 提交、I/O 消费

---

## 📊 实验阶段概览

| 阶段 | 实验 | 目标 | 周数 | 关键闸门 |
|------|------|------|------|---------|
| 1 | E1-dev, E3-dev | 快速验证三类分类价值 | 1-2 | I/O 减少 ≥20%？ |
| 2 | E2, E4, E5, E6 | 消融各组件的必要性 | 1-2 | 所有组件都有意义？ |
| 3 | E7（基线） | 与 DiskANN/SPANN 竞争 | 1-2 | iso-recall 竞争？ |
| 4 | E8, E9 | 规模验证与真实数据 | 1-2 | 规模可扩展？ |

---

## 🚀 首先要运行的 3 个实验

为了尽快验证核心想法：

1. **E1-dev on SIFT1M**（<1 小时）
   - 构建索引，运行 BoundFetch vs Eager-Fetch
   - 检查：I/O 体积减少是否达到 20%+？

2. **E3-dev on SIFT1M**（<2 小时）
   - 分类消融：完整 vs 二元 vs 无分类 vs 贪心
   - 检查：3 类是否优于简单二元分类 10%+？

3. **E2-quick on SIFT1M**（<2 小时）
   - 重叠率分析：prefetch_depth 敏感性
   - 检查：重叠率是否 >60%？

**如果这 3 个实验都通过**，可以确信核心机制有效，继续进行规模化评估。

---

## 📝 英文版本

所有文档都有对应的英文版本：
- FINAL_PROPOSAL.md
- REVIEW_SUMMARY.md
- REFINEMENT_REPORT.md
- EXPERIMENT_PLAN.md
- EXPERIMENT_TRACKER.md
- PIPELINE_SUMMARY.md

---

## 💾 文件清单

```
refine-logs/
├── FINAL_PROPOSAL.md                    # 英文版
├── FINAL_PROPOSAL_CN.md                 # 中文版 ✓
├── REVIEW_SUMMARY.md                    # 英文版
├── REVIEW_SUMMARY_CN.md                 # 中文版 ✓
├── REFINEMENT_REPORT.md                 # 英文版
├── REFINEMENT_REPORT_CN.md              # 中文版 ✓
├── EXPERIMENT_PLAN.md                   # 英文版
├── EXPERIMENT_PLAN_CN.md                # 中文版 ✓
├── EXPERIMENT_TRACKER.md                # 英文版
├── EXPERIMENT_TRACKER_CN.md             # 中文版 ✓
├── PIPELINE_SUMMARY.md                  # 英文版
├── PIPELINE_SUMMARY_CN.md               # 中文版 ✓
└── README_CN.md                         # 本文件
```

---

## ✅ 检查清单

开始实验前的准备：

- [ ] 阅读 FINAL_PROPOSAL_CN.md 理解方法
- [ ] 确认硬件（NVMe SSD, AVX-512, 64GB RAM）
- [ ] 下载数据集（至少 SIFT1M）
- [ ] 构建项目（CMake, C++17）
- [ ] 运行 E1-dev 作为首个验证
- [ ] 根据 EXPERIMENT_TRACKER_CN.md 跟踪进度

---

## 📞 问题排查

**Q: 如何快速理解 BoundFetch 的核心思想？**
A: 阅读 FINAL_PROPOSAL_CN.md 的第 2-3 节（方法论文 + 三类分类）。

**Q: 为什么要做这么多消融实验？**
A: 确保每个设计决策都有实证支持。E3, E4, E5, E6 分别验证分类、布局、多阶段、CRC 的必要性。

**Q: 如果早期实验失败怎么办？**
A: 按 EXPERIMENT_PLAN_CN.md 的"决策闸门"规则，决定是调优参数还是重新评估设计。

**Q: 能否跳过某些实验？**
A: 可以，但风险自担。标记为"必须运行"的实验（E1, E3, E4, E7）是关键。

---

**准备好开始了吗？** → 从 E1-dev 开始：在 SIFT1M 上运行 BoundFetch vs Eager-Fetch！
