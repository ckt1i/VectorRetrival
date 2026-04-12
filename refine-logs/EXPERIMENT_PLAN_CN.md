# 实验计划：BoundFetch

**日期**：2026-04-10（v3: 基于初步实验 + 用户反馈修正）
**基于**：FINAL_PROPOSAL_CN.md v3、REVIEW_SUMMARY_CN.md v3

---

## 0. 要验证的声称

| ID | 声称 | 实验 | 优先级 |
|----|------|------|--------|
| C1 | SafeOut 剪枝消除 90%+ 候选无效 I/O，减少总 I/O 体积 40-60% | E1, E3 | 关键 |
| C2 | 单线程重叠调度器实现 >80% CPU-I/O 重叠率 | E2 | 关键 |
| C3 | 协同存储布局实现 2-3x 更低端到端延迟 vs 朴素布局 | E4 | 关键 |
| C4 | 多阶段量化（FastScan→ExRaBitQ）紧缩 Uncertain 率 | E5 | 重要 |
| C5 | CRC 动态阈值实现聚类早停（top_k 小时最强） | E6 | 重要 |
| C6 | search+payload 端到端优于现有系统（含图索引 payload 后取） | E7 | 关键 |
| C7 | SafeIn 在特定条件下可显现额外 I/O 收益 | E8 | 探索性 |

---

## 1. 硬件与环境

### 必需设置
- **机器**：单节点服务器，NVMe SSD（≥2TB）
- **CPU**：x86-64 支持 AVX-512（Intel Sapphire Rapids / AMD Zen 4+）
- **内存**：64GB RAM（索引元数据在内存中，数据在磁盘）
- **OS**：Linux 6.x 支持 io_uring（内核 ≥5.1，建议 ≥6.1）
- **页缓存**：通过 `echo 3 > /proc/sys/vm/drop_caches` 在运行间控制

### 软件依赖
- 编译：CMake、GCC 12+ / Clang 16+（C++17、AVX-512）
- 基线：SPANN（官方实现，同为 IVF 系）、FAISS IVF-PQ（磁盘模式）
- 参考基线：DiskANN（仅用于 search+payload 端到端对比，不做纯搜索对比）
- 指标：perf、iostat、blktrace 用于 I/O 分析

---

## 2. 数据集（修正版：以真实 payload 为主）

### 主要评估数据集（真实 payload）

| 数据集 | 向量数 | 维度 | Payload 类型 | Payload 大小 | 用途 |
|--------|--------|------|-------------|-------------|------|
| **MS MARCO Passage** | 8.8M | 768 | 段落文本 | ~200B avg | RAG 标准基准，小 payload |
| **Natural Questions** | 21M | 768 | Wikipedia 段落 | ~500B avg | 大规模文本检索 |
| **COCO** | 100K-330K | 768 | 原始图片(JPEG) | 10-500KB var | 多模态，大+可变 payload |
| **Amazon Products** | 1.4M | 768 | 产品描述+图片 | 1-50KB var | 电商场景，中等 payload |
| **LAION-5M subset** | 5M | 768 | 图片URL+描述 | ~1KB avg | 大规模多模态 |

### 辅助评估数据集（可控变量）

| 数据集 | 向量数 | 维度 | Payload 配置 | 用途 |
|--------|--------|------|-------------|------|
| **Deep1M** | 1M | 96 | 合成 1KB/10KB/100KB | 纯向量搜索基线（已有数据） |
| **Deep10M** | 10M | 96 | 合成 10KB | 中等规模可控实验 |
| **SIFT10M** | 10M | 128 | 合成 10KB | 可控消融实验 |

### 数据集选择原则
- **真实 payload 为主**：至少 60% 的实验使用真实 payload 数据集
- **可控变量为辅**：合成 payload 仅用于隔离 payload 大小对性能的影响
- **已有数据优先**：COCO 100K 和 Deep1M 已经有初步实验数据，作为开发迭代基础

---

## 3. 实验描述

### E1：主要结果——端到端性能（C1, C6）

**目标**：在真实 payload 基准上演示 BoundFetch 的 recall-延迟-IO 权衡。

**配置**：
- 数据集：MS MARCO Passage、COCO 330K、Deep10M (10KB合成)
- 参数：top_k ∈ {10, 20, 50, 100}，nprobe ∈ {16, 32, 64, 128}
- 查询：每配置 1000-10000 个
- 指标：Recall@K、QPS、P50/P95/P99 延迟、总 I/O 字节、I/O 操作数

**对比系统**：
- **BoundFetch**（完整系统）
- **BoundFetch-NoPayload**（仅向量搜索——上界）
- **Eager-Fetch**（对所有 Uncertain 候选取完整记录）
- **Sequential-Fetch**（先搜索，再取 Top-K payload）

**关键图表**：
- Recall vs QPS（Pareto 前沿），payload 大小作参数
- Recall vs I/O 体积（每查询字节）
- I/O 分解柱图：SafeOut 跳过、Uncertain 向量读、Uncertain payload 读、浪费读

**决策闸门**：BoundFetch 在 iso-recall 上 I/O 体积减少 ≥30% vs Eager-Fetch？

---

### E2：I/O 重叠分析（C2）

**目标**：量化单线程 CPU-I/O 重叠有效性。

**配置**：
- 数据集：MS MARCO Passage（真实 payload）、Deep10M (10KB合成)
- 固定：top_k=10，recall 目标=95%
- 变化：nprobe ∈ {16, 32, 64}，prefetch_depth ∈ {1, 2, 4, 8, 16, 32}

**指标**：
- I/O 等待时间 / 总时间 → 重叠率
- 延迟分解：probe_time, rerank_time, io_wait_time, total_time
- I/O 带宽利用率

**关键图表**：
- 重叠率 vs prefetch_depth（折线图）
- 堆叠柱图：时间分解

**决策闸门**：重叠率 >60% 在 depth≥8？

---

### E3：分类消融（C1）—— **必须运行**

**目标**：隔离 SafeOut 剪枝相对其他策略的价值。

**配置**（MS MARCO Passage, top_k=10）：
| 配置 | SafeOut | SafeIn | Uncertain 处理 | 描述 |
|------|--------|--------|---------------|------|
| 完整三类 | 是 | 是 | 仅向量 → 按需 payload | BoundFetch 完整 |
| 仅 SafeOut | 是 | 否 | 仅向量 → 按需 payload | 无 SafeIn |
| 无分类 | 否 | 否 | 所有候选读向量 → 按需 payload | 无 SafeOut/SafeIn |
| 贪心预取 | 否 | 否 | 所有候选读完整记录 | 最大 I/O |

**指标**：Recall@10、QPS、I/O 字节/查询、浪费 I/O 字节

**决策闸门**：完整三类 vs 仅 SafeOut 差距 <5% → 论文可简化为二类叙事

---

### E4：存储布局消融（C3）—— **必须运行**

**目标**：验证双文件共存布局的价值。

**布局变体**：
| 布局 | 向量存储 | Payload 存储 | 地址 |
|------|---------|-----------|------|
| 共存（ours） | .dat 中与 payload | .dat 中与向量 | 地址列在 .clu 中 |
| 分离 | 分离 .vec 文件 | 分离 .payload 文件 | 两个地址列 |

**数据集**：COCO（真实可变 payload）、MS MARCO（真实小 payload）、Deep10M（合成控制变量 1KB/10KB/100KB）

**指标**：端到端延迟、I/O 操作/查询、I/O 字节/查询

**决策闸门**：共存在真实 payload 时 ≥1.5x 优于分离？

---

### E5：多阶段量化消融（C4）

**目标**：展示 Stage 2（ExRaBitQ）对 Uncertain 率的紧缩价值。

**配置**：
| 配置 | Stage 1 | Stage 2 | 描述 |
|------|---------|---------|------|
| 仅 1-bit | FastScan 1-bit | 无 | 单阶段 |
| 1+2 bit | FastScan 1-bit | ExRaBitQ 2-bit | 双阶段 |
| 1+4 bit | FastScan 1-bit | ExRaBitQ 4-bit | 双阶段（当前默认） |

**数据集**：Deep1M（已有基线）、COCO（高维）

**指标**：SafeOut 率、Uncertain 率、recall@10、QPS、Stage 2 时间开销

**决策闸门**：ExRaBitQ Uncertain 减少 <10% → 降级为可选优化

---

### E6：CRC 早停消融 + top_k 敏感性（C5）—— **关键新增实验**

**目标**：分析 CRC 早停与 top_k 的反向关系。

**配置**：
- CRC 开 vs CRC 关
- top_k ∈ {10, 20, 50, 100}
- nprobe ∈ {32, 64, 128}

**数据集**：Deep1M（已有 top_k=10/20 数据）、MS MARCO

**指标**：
- 实际探测聚类 / 请求 nprobe
- 早停率
- SafeOut 率（在相同 nprobe 下）
- 延迟 vs top_k（展示两个机制的交互）

**关键图表**：
- 双 Y 轴折线图：SafeOut 率（左）+ 早停率（右） vs top_k
- 延迟 vs top_k 柱图（分解为 probe 时间 + I/O 时间）

**初步锚点**：
```
Deep1M: top_k=10 → SafeOut 93.79%, 早停 100%, 27/100 clusters
        top_k=20 → SafeOut 97.91%, 早停 21%,  85.64/100 clusters
```

**决策闸门**：CRC 关 vs 开在 top_k=10 时延迟差距 ≥30%？

---

### E7：基线比较（C6）—— **必须运行**

**目标**：在 search+payload 端到端指标上定位 BoundFetch。

**基线**：
1. **SPANN**（官方实现，同为 IVF 系——最公平的比较）
2. **FAISS IVF-PQ**（磁盘模式，内存映射索引）
3. **DiskANN**（图索引——仅做 search+payload 端到端比较，**不做纯向量搜索对比**）
4. **BoundFetch**（我们的系统）

**数据集**：MS MARCO Passage、COCO 330K、Deep10M (10KB)

**协议**：
- 各系统构建索引、热身、运行查询
- 系统间丢弃页缓存
- 报告两个指标：
  - **仅搜索延迟**：仅与 SPANN/FAISS 对比
  - **search+payload 端到端延迟**：与所有系统对比
    - DiskANN/SPANN/FAISS：搜索完成后 + 顺序 pread 取 Top-K payload
    - BoundFetch：payload 获取融入搜索流程

**关键图表**：
- Recall@10 vs search+payload QPS（Pareto 曲线，所有系统同图）
- 柱图：仅搜索延迟 vs search+payload 延迟（展示 payload 额外成本）

**决策闸门**：BoundFetch search+payload 端到端优于所有基线的 search+payload？

---

### E8：SafeIn 激活条件探索（C7）—— **新增探索性实验**

**目标**：系统性探索 SafeIn 可能显现价值的参数空间。

**变化维度**：
| 维度 | 值 | 预期影响 |
|------|-----|---------|
| top_k | 10, 20, 50, 100, 200 | top_k↑ → d_k 更宽松 → SafeIn 条件更易满足 |
| bits | 1, 2, 4, 8 | bits↑ → margin 更小 → SafeIn 条件更易满足 |
| d_k 校准分位数 | P90, P95, P99 | 分位数↓ → d_k 更紧 → SafeIn 条件更易满足但 recall 风险 |
| payload 大小 | 1KB, 10KB, 100KB | 影响 SafeIn 带来的 I/O 节省量级 |

**数据集**：Deep1M（快速迭代）、MS MARCO（验证）

**指标**：
- SafeIn 率（%）
- SafeIn 命中的 Payload I/O 节省量
- Recall 影响（d_k 校准变化时）

**关键图表**：
- 热力图：SafeIn 率 @ (top_k × bits) 矩阵
- 折线图：SafeIn 率 vs d_k 校准分位数

**决策闸门**：
- SafeIn 率在任意配置下 ≥5% → 可作为论文贡献点
- SafeIn 率在所有配置下 <1% → 降级为"future work"并在论文中诚实讨论

---

### E9：可扩展性研究

**目标**：展示递增规模下的行为。

**数据集**：Deep1M → Deep10M → Natural Questions (21M)
**固定**：top_k=10, recall 目标=95%

**指标**：QPS、延迟、I/O 字节/查询、索引构建时间

---

### E10：真实多模态端到端（可选）

**目标**：完整的 RAG/多模态检索 demo。

**场景 1 (RAG)**：MS MARCO，查询 → 检索最相关段落 + 返回文本内容
**场景 2 (多模态)**：COCO，文本查询 → 检索最相似图片 + 返回原始 JPEG

---

## 4. 运行顺序和决策闸门

```
第 1 阶段：快速验证（1 周）
├── Deep1M 上补充 top_k 扫描（E6 快速版，已有部分数据）
├── COCO 上的 E1 快速版（已有索引）
├── E3 分类消融（Deep1M，快速）
└── 闸门：SafeOut 剪枝 ≥30% I/O 减少？
    ├── 是 → 继续
    └── 否 → 检查阈值

第 2 阶段：真实数据集 + 核心消融（2 周）
├── 准备 MS MARCO、Amazon Products 数据集
├── E1 主要结果（MS MARCO + COCO）
├── E4 布局消融
├── E5 多阶段消融
├── E6 CRC 早停 + top_k 敏感性完整版
├── E8 SafeIn 激活条件探索
├── E2 重叠分析
└── 闸门：所有组件贡献有意义？SafeIn 是否值得突出？
    ├── SafeIn ≥5% → 保留三类叙事
    └── SafeIn <1% → 以 SafeOut 为主叙事，SafeIn 为 future work

第 3 阶段：基线比较（2 周）
├── SPANN、FAISS 基线设置
├── DiskANN 基线设置（仅 search+payload 端到端）
├── E7 基线比较（MS MARCO、COCO、Deep10M）
└── 闸门：search+payload 端到端竞争？
    ├── 是 → 规模扩展
    └── 否 → 分析差距

第 4 阶段：规模与润色（1 周）
├── E9 可扩展性
├── E10 真实端到端 demo（可选）
└── 生成论文图表
```

---

## 5. 计算预算估计

| 实验 | CPU 小时 | 存储 | 优先级 |
|------|---------|------|--------|
| E1 主要结果 | ~30 | 200GB | 关键 |
| E2 重叠分析 | ~8 | 50GB | 关键 |
| E3 分类消融 | ~12 | 50GB | 关键 |
| E4 布局消融 | ~20 | 100GB | 关键 |
| E5 多阶段消融 | ~10 | 50GB | 重要 |
| E6 CRC+top_k 敏感性 | ~12 | 50GB | 重要 |
| E7 基线比较 | ~40 | 400GB | 关键 |
| E8 SafeIn 探索 | ~16 | 50GB | 探索性 |
| E9 可扩展性 | ~20 | 300GB | 重要 |
| E10 真实端到端 | ~5 | 50GB | 可选 |
| **合计** | **~173 CPU 小时** | **~1.3TB 峰值** | |

---

## 6. 论文中报告的关键指标

### 表 1：主要结果（真实 payload）
- Recall@{10,50,100} × QPS × 延迟 P50/P99 × I/O 字节/查询
- 行：BoundFetch, Eager-Fetch, Sequential-Fetch, SPANN, FAISS, DiskANN(search+payload)
- 列：MS MARCO, COCO, Deep10M

### 表 2：I/O 分解
- SafeOut 跳过、Uncertain 向量读、Uncertain payload 读、浪费读

### 表 3：消融总结
- 完整 vs 仅SafeOut vs 无分类 vs 无ExRaBitQ vs 无CRC vs 无重叠 vs 分离布局

### 表 4：top_k 敏感性分析（新增）
- top_k × SafeOut率 × 早停率 × 延迟 × recall

### 表 5 (可选)：SafeIn 激活条件
- top_k × bits × d_k分位数 → SafeIn 率

### 图 1：系统架构
### 图 2：Recall-QPS Pareto 曲线 (search+payload 端到端, E7)
### 图 3：延迟分解堆叠柱 (E2)
### 图 4：SafeOut 剪枝价值 (E3)
### 图 5：top_k 敏感性双 Y 轴图 (E6)
### 图 6：SafeIn 探索热力图 (E8, 可选)
### 图 7：可扩展性 (E9)
