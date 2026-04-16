# 正式 Baseline 策略

日期：2026-04-16

## 1. 目标

这套 baseline 方案服务于同一个问题：

在 `向量搜索 + 原始 payload 协同读取` 的真实服务路径里，BoundFetch 相对于同类 IVF 系方法、图索引方法、以及不同存储后端，究竟处在什么位置。

## 2. 当前方法的有效创新点

基于当前仓库实现、`architecture.md` 和 `FINAL_PROPOSAL_CN.md`，BoundFetch 目前最需要被验证的创新点不是单一 ANN 算法本身，而是以下四项组合：

| 编号 | 当前主张 | 需要什么 baseline 来支撑 |
|------|----------|--------------------------|
| I1 | 系统目标是 `搜索 + payload` 的 E2E 路径，而不是纯 ANN latency | 需要主表之外的 E2E 表和 payload backend 表 |
| I2 | IVF 框架下，`CRC/ConANN 风格控制 + RaBitQ/量化估计 + rerank` 共同形成更优 tradeoff | 需要多个 IVF+rerank baseline，而不是只有一个 IVFPQ |
| I3 | `.clu` preload 和后续 CPU 优化是在现有系统结构上做最小干预 | 需要保持同一 storage/payload 语义，不要用纯内存 ANN 来代替 |
| I4 | BoundFetch 的潜在价值不止是查询速度，还包括构建时间、启动代价和未来动态更新兼容性 | 需要 build/startup 成本表，DiskANN 作为参考而不是唯一主目标 |

## 3. 比较层次

正式实验按四层组织。

### Layer A：主搜索核对比

目标：
在同一数据、同一 encoder、同一 top-k、同一 warm 协议下，比较 BoundFetch 与同类 IVF 系检索方法。

主表必须包含：

- BoundFetch
- Faiss IVFPQ + rerank
- Faiss IVFPQR
- Faiss IVFResidualQuantizer 或 IVFLocalSearchQuantizer

可选增强项：

- ConANN + Faiss IVF
- 外部 IVF + RaBitQ / Extended-RaBitQ

### Layer B：强参考方法

目标：
保留图索引上界，但不让它吞掉主故事。

本层包含：

- DiskANN

定位：

- 作为强参考和优化目标
- 作为 build/startup 成本对照
- 不作为“是否值得发表”的唯一裁决标准

### Layer C：存储后端对比

目标：
把搜索核效应和 payload 后端效应拆开。

后端固定集合：

- FlatStor
- Lance
- Parquet

主建议：

- 主表默认只放 1 个主后端
- 其余后端放单独的 storage ablation 表

### Layer D：E2E 与成本表

目标：
说明 BoundFetch 是系统方案，而不是单个 ANN 核心。

必须输出：

- `recall@k`
- `e2e_ms / p50 / p95 / p99`
- `payload_ms`
- `build_time`
- `peak_rss`
- `index_bytes`
- `preload_time`
- `preload_bytes`

## 4. 主表与附录的切分

### 主表必须回答的问题

1. BoundFetch 相对同类 IVF+rerank 系方法是否占优。
2. BoundFetch 在 E2E `search + payload` 场景下是否仍有系统优势。
3. DiskANN 即使更强，BoundFetch 是否仍占据更好的 build/startup/integration 点。

### 附录可以回答的问题

1. 是否存在更多多模态或更大规模数据集上的同方向结论。
2. `FlatStor / Lance / Parquet` 的差异是否稳定。
3. 图索引和 IVF 系方法在不同 payload 大小时的差异如何变化。

## 5. 本轮实验不做什么

- 不做完整的 `search x storage x dataset` 笛卡尔积。
- 不把 DiskANN 从对比中删掉。
- 不只用 COCO 100K 一套数据讲完全部故事。
- 不再把只有 Faiss IVFPQ 一个 IVF baseline 的配置称为“正式 baseline”。

## 6. 推荐的论文叙事

推荐将主线表述为：

BoundFetch 的贡献不在于“一个 IVF 方法绝对打赢所有图索引”，而在于：

1. 在真实的 `搜索 + payload` 路径中，它比同类 IVF+rerank 系方案更完整；
2. 它在保持较低构建和启动成本的前提下，逼近强参考图索引；
3. 它的存储后端和 payload 协同设计让系统对比更有意义。
