## Context

当前查询路径中的 `.clu` 访问已经从逐条同步读取演进为基于 `io_uring` 的滑动窗口 cluster block 预取。相关代码主要落在：

- `src/storage/cluster_store.cpp`
- `include/vdb/storage/cluster_store.h`
- `src/query/overlap_scheduler.cpp`
- `include/vdb/query/search_context.h`
- `benchmarks/bench_e2e.cpp`

现状的关键特征是：

- `ClusterStoreReader` 已提供 `.clu` 文件描述符、lookup table 查询和 `ParseClusterBlock()`
- `OverlapScheduler` 通过 `prefetch_depth / refill_threshold / refill_count` 维护滑动窗口式 `CLUSTER_BLOCK` I/O
- 查询期仍然需要为 `.clu` 发起 I/O、等待 CQE、解析 block，再把 `ParsedCluster` 放入 `ready_clusters_`
- 最新 warm benchmark 已经表明 `io_wait` 很低，继续挖掘通用 I/O overlap 的收益空间有限

用户现在要验证的不是“滑窗还能不能更快一点”，而是一个更直接的结构变化：在查询开始前一次性读取整个 `.clu` 文件，并将量化向量和地址相关元数据常驻在内存里，然后和旧的滑窗路径做查询速度对比。

## Goals / Non-Goals

**Goals:**
- 提供一个全量 `.clu` preload 模式，在 warmup 或 index open 阶段一次性读取整个 `.clu` 文件
- 让查询阶段直接复用常驻的 cluster 数据，不再为 `.clu` 发起 `CLUSTER_BLOCK` I/O
- 保持现有 payload / `.data` 读取路径不变
- 保留滑动窗口 `.clu` 读取路径作为可回退模式和对照基线
- 让 benchmark 能在相同参数下对比 `window` 与 `full-preload` 两种模式的查询速度

**Non-Goals:**
- 不预加载 payload bytes 或 `.data` 文件主体
- 不重新设计 `io_uring` 的向量 / payload 提交机制
- 不要求新的磁盘格式
- 不把所有搜索路径都重写成新的数据结构

## Decisions

### 1. 由 `ClusterStoreReader` 持有全量 `.clu` 常驻状态

**Decision**
- 将整份 `.clu` 文件的 preload 所有权放在 `ClusterStoreReader`，并由 `Segment` 暴露只读访问接口给查询层。

**Rationale**
- `.clu` fd、lookup table 和 block 解析逻辑本来就由 `ClusterStoreReader` 管理
- 把 preload 放在查询层会让生命周期、回退和 benchmark 控制更分散
- 由存储层持有，更容易复用已有的 block location 和 parse 逻辑

**Alternatives considered**
- 由 `OverlapScheduler` 直接在启动时把 `.clu` 读入自己的私有缓存
  - 否决：查询层不应拥有文件级缓存，且会让 benchmark/生命周期控制更难维护
- 单独新增一个 preload service
  - 否决：改动面太大，对当前目标来说过度设计

### 2. 全量读取后建立“resident cluster view”，查询期直接走内存视图

**Decision**
- preload 阶段一次性读取整个 `.clu` 文件到常驻 buffer，并为每个 cluster 建立可直接 probe 的 resident view。
- resident view 至少包含：
  - codes 起始指针
  - code entry size
  - record 数
  - 已解码地址表
  - 其他 `ParsedCluster` 需要的只读元数据

**Rationale**
- 只把整文件 buffer 读进内存但在查询期继续调用 `ParseClusterBlock()`，仍会保留解析开销
- 预先把 cluster 视图物化好，才能最大化查询阶段收益并让 before/after 对比更干净
- 这样也更贴近用户要验证的方向：查询前准备好 cluster 侧状态

**Alternatives considered**
- 只 preload 原始 `.clu` 文件，查询期按 cluster 在内存里现 parse
  - 否决：仍会把解析工作留在热路径上
- 将每个 cluster 复制成独立 `ParsedCluster.block_buf`
  - 否决：会把一次性 preload 变成二次内存复制，收益不明显

### 3. 查询路径引入显式 `.clu` 读取模式开关

**Decision**
- 增加显式模式，例如 `clu_read_mode = window | full_preload`
- `full_preload` 模式下，查询流程不得提交任何 `CLUSTER_BLOCK` I/O
- `window` 模式继续保留现有 `prefetch_depth` 驱动的滑动窗口逻辑

**Rationale**
- 需要稳定、可复现的 before/after 对照实验
- 需要保留回退路径，避免 preload 模式一旦不稳定就堵死查询流程
- benchmark 输出也需要携带模式字段，才能可靠回填 CSV 和分析文档

**Alternatives considered**
- 直接替换旧逻辑，不保留滑窗路径
  - 否决：无法做同代码库下的公平前后对比，也不利于回滚

### 4. Benchmark 对照以“相同召回语义 + 相同参数”作为硬约束

**Decision**
- e2e benchmark 必须支持在相同 `dataset / queries / topk / nprobe / crc-alpha / epsilon / nlist` 下，对比 `window` 和 `full_preload`
- 输出中必须同时记录查询延迟收益和 preload 成本

**Rationale**
- 用户要验证的是读取策略变化是否提高查询速度，而不是换参数换来更快结果
- preload 不是免费优化，必须把 preload time 和额外内存带出来

**Alternatives considered**
- 只记录查询延迟，不记录 preload 开销
  - 否决：会让结论不完整，难以判断实际部署价值

## Risks / Trade-offs

- [风险] `.clu` 文件过大，full preload 导致内存占用明显增长  
  → Mitigation：benchmark 必须输出 preload bytes / resident RSS；保留 `window` 模式作为回退。

- [风险] preload 后查询变快，但启动时间明显变差  
  → Mitigation：单独记录 preload time，并把“查询期收益 vs 启动期开销”作为结果的一部分，而不是隐藏掉。

- [风险] preload 后仍然收益有限，说明当前瓶颈不在 `.clu` I/O  
  → Mitigation：保持设计最小化，若结果不佳即可快速停止这条优化线。

- [风险] resident view 与现有 `ParsedCluster` / `ClusterProber` 接口不完全兼容  
  → Mitigation：优先复用 `ParsedCluster` 可表达的数据布局，必要时增加一个轻量只读 view 适配层，而不是重写 probe 算法。

- [风险] 两种模式并存增加代码复杂度  
  → Mitigation：把模式分叉尽量集中在 cluster 获取层；payload 路径、rerank 路径和结果收集逻辑不分叉。

## Migration Plan

1. 在 `ClusterStoreReader` 中加入 `.clu` 全量 preload 所需的数据结构和接口。
2. 在查询配置中加入 `.clu` 读取模式开关，并默认保持现有 `window` 行为不变。
3. 在 `OverlapScheduler` 中接入 `full_preload` 路径：
   - 直接从 resident cluster view 获取 cluster 数据
   - 跳过 `CLUSTER_BLOCK` 提交、poll 和 `ready_clusters_` 依赖
4. 在 `bench_e2e` 中增加模式参数和结果导出字段。
5. 先跑一次旧模式，再跑一次新模式，比较查询速度提升与 preload 成本。

回滚策略：
- 通过模式开关回退到 `window`
- 若 preload 路径不稳定，不删除旧实现

## Open Questions

- resident view 最终是直接复用 `ParsedCluster`，还是引入一个更轻的 `ResidentClusterView` 再适配给 probe？
- preload 触发点是 `Segment::Open()` 之后立即执行，还是 benchmark warmup 阶段显式执行更合适？
- preload 时是否要顺手做地址 decode 和其他轻量解析的完整物化，还是保留一部分只读 lazy decode？
