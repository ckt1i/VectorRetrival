# 管道总结

**问题**：磁盘向量搜索系统在交付向量+原始数据时，要么顺序获取增加一轮 I/O 延迟，要么贪心预取浪费 90%+ 的 I/O 带宽。

**最终方法论述**：利用 RaBitQ 理论误差界作为置信信号进行三类 I/O 调度——SafeOut 剪枝（90-98%）消除无效 I/O、SafeIn 预取（激活条件待探索）加速高置信候选、Uncertain 按需验证——通过 io_uring 单线程重叠调度在双文件布局上执行。

**最终评审结论**：就绪（附条件：SafeIn 需实验验证激活条件）

**日期**：2026-04-10（v3）

## 最终可交付物
- 提案：`refine-logs/FINAL_PROPOSAL_CN.md` (v3)
- 评审总结：`refine-logs/REVIEW_SUMMARY_CN.md` (v3)
- 精化报告：`refine-logs/REFINEMENT_REPORT_CN.md` (v3)
- 实验计划：`refine-logs/EXPERIMENT_PLAN_CN.md` (v3)
- 实验追踪：`refine-logs/EXPERIMENT_TRACKER_CN.md` (v3)

## 贡献快照
- **主导贡献**：误差界驱动的 I/O 调度——SafeOut 剪枝消除 90-98% 候选的无效 I/O
- **探索性贡献**：SafeIn 预取机制——理论完备，待实验探索激活条件（大 top_k、高精度量化、大 payload）
- **支撑贡献**：双文件存储布局、io_uring 单线程重叠调度、多阶段 FastScan→ExRaBitQ
- **明确舍弃**：图索引（不适配批量 I/O 调度）、多线程（正交工程问题）、GPU、分布式

## 必须证明的声称
- C1: SafeOut 剪枝减少 I/O 体积 40-60% vs 贪心预取
- C2: 单线程重叠调度器 >80% CPU-I/O 重叠率
- C3: 双文件共存布局 2-3x 优于分离布局
- C4: ExRaBitQ Stage 2 紧缩 Uncertain 率
- C5: CRC 早停（top_k=10 时 73%，随 top_k 变化）
- C6: search+payload 端到端优于 SPANN/FAISS/DiskANN
- C7: SafeIn 在特定条件下 ≥5% 激活率（探索性）

## 初步实验关键发现
```
SafeOut 率:  90-98%（Stage 1，随 top_k 变化）
SafeIn 率:   ≈ 0%（所有当前配置）
CRC 早停:    top_k=10 → 73%, top_k=20 → 21%（反向关系）
top_k 权衡:  SafeOut↑ vs 早停↓（值得深入分析的有趣现象）
```

## 首先启动的实验
1. **E6-快速**：Deep1M 上 top_k={50,100} 补充数据，验证 SafeOut/早停趋势
2. **E3-快速**：Deep1M 分类消融（完整 vs 仅SafeOut vs 无分类 vs 贪心），验证 I/O 减少
3. **E8-快速**：Deep1M 上 bits={1,2,4,8} × top_k={10,50,100} 的 SafeIn 率扫描

## 主要风险
- **SafeIn 全面失效**：缓解——即使 SafeIn 为 0，SafeOut 剪枝仍是有力贡献；保留 SafeIn 为框架完备性和 future work
- **CRC 早停 top_k 敏感性**：缓解——如实报告，分析两个机制的互补关系
- **图索引纯搜索优势**：缓解——不做纯搜索比较，聚焦 search+payload 端到端
- **真实数据集准备耗时**：缓解——用已有 Deep1M/COCO 快速验证后再扩展

## 下一步行动
- 运行 E6-快速和 E3-快速（使用已有 Deep1M 数据，<半天）
- 开始准备 MS MARCO Passage 数据集
- 进入 `/run-experiment`
