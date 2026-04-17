## Context

当前代码已经具备 `.clu` 全量 preload 的底层能力：`ClusterStoreReader::PreloadAllClusters()` 会把整个 `.clu` 文件读入 `resident_file_buffer_`，并为每个 cluster 解析 `fastscan_blocks`、`exrabitq_entries` 与 `decoded_addresses`。但查询层的 `full_preload` 仍然保留了两类不应出现在 warm steady-state 路径中的动作：

- preload 在 `OverlapScheduler::Search()` 中懒触发，首次查询仍承担启动期开销
- 每个 query 会通过 `ResidentClusterView::ToParsedCluster()` 重新物化 `ParsedCluster`，并复制 `decoded_addresses`

这导致现有 `full_preload` 并不是一个真正的 resident compressed-plane 查询路径。与此同时，现有 IVF-RaBitQ baseline 默认把 compressed index 作为常驻内存结构直接搜索，因此当前对比并不公平，也无法量化“如果 `.clu` 完全常驻且 query 期零准备动作，BoundFetch 的 steady-state 搜索速度能提升多少”。

## Goals / Non-Goals

**Goals:**
- 将 `.clu` `full_preload` 从“查询期懒触发 resident 缓存”改造成“启动期 resident probe-ready path”
- 在 preload 阶段一次性读取并解析所有 cluster，将量化向量视图和原始 block 地址物化为全局常驻的 probe-ready 结构
- 在 `full_preload` 模式下，查询热路径不再发生任何 `.clu` 读取、cluster parse、resident-to-ParsedCluster 重建或地址复制
- 为 benchmark 增加 resident 前后对照实验，固定同一组 warm 参数 before/after 各测一遍，并输出搜索速度提升、preload 时间和常驻内存占用

**Non-Goals:**
- 不把 raw vectors 或 payload bodies 一并 preload 到内存
- 不修改 BoundFetch 的核心 pruning 语义（FastScan / Stage 2 / CRC / early stop）
- 不把 baseline 改造成与 BoundFetch 完全同构的 disk-serving 系统
- 不在这次 change 中重写 `AsyncReader` 或 io_uring 提交路径

## Decisions

### 1. 将 resident compressed-plane preload 前移到 benchmark/index warm-up 阶段

选择：
- `Search()` 不再负责首次 preload
- 由 benchmark 或显式 warm-up 阶段在查询开始前调用 `PreloadAllClusters()`

原因：
- preload 属于启动期成本，不应记入 steady-state query latency
- 可以更清楚地区分 `preload_time_ms` 与 `e2e_ms`

备选：
- 继续在 `Search()` 中懒触发一次
- 否决原因：首次 query 仍然掺杂启动期成本，不利于和 resident baseline 公平对比

### 2. 在存储层新增全局 resident probe-ready cluster 结构，而不是 query 期临时 `ParsedCluster`

选择：
- 将 `ResidentClusterView` 扩展为可直接被 probe 路径消费的结构
- 避免通过 `ToParsedCluster()` 在每个 query 中复制 `decoded_addresses`

原因：
- 当前 query 期最大的残余冗余就是 resident view 到 `ParsedCluster` 的重建
- `decoded_addresses` 的逐 query 拷贝会放大 CPU 和内存带宽成本

备选：
- 继续保留 `resident_query_clusters_` 作为每 query cache
- 否决原因：它仍然是 query-local materialization，不符合 resident warm path 的目标

### 3. `full_preload` 模式下彻底绕过 cluster I/O 路径

选择：
- `PrefetchClusters()` 在 resident mode 下直接标记无 cluster inflight
- `ProbeAndDrainInterleaved()` 只从全局 resident cluster 结构取 probe 输入
- `ready_clusters_`、`SubmitClusterRead()`、`DispatchCompletion(CLUSTER_BLOCK)` 在 resident mode 下不再参与查询热路径

原因：
- 只 preload 文件但还保留 cluster I/O 状态机，无法真正消掉调度成本
- resident mode 的收益必须来自“热路径零 cluster I/O 和零 parse”

备选：
- preload 文件，但保留现有 ready queue 语义
- 否决原因：仍会保留大量 query 期 bookkeeping

### 4. benchmark 明确采用 before/after 对照，而不是只记录 resident 模式单点

选择：
- 固定同一组 warm 参数跑两次：
  - before：现有 `full_preload`
  - after：resident probe-ready warm path
- 输出 resident 指标和速度提升比

原因：
- 这次 change 的目的不是引入新模式本身，而是量化它能消掉多少查询期成本
- before/after 是最直接、最可解释的验证方式

推荐第一轮固定参数：
- dataset = `coco_100k`
- queries = `1000`
- topk = `10`
- nlist = `2048`
- nprobe = `200`
- crc-alpha = `0.02`
- epsilon-percentile = `0.99`

备选：
- 直接扫完整 Pareto
- 否决原因：在 resident path 尚未验证前，全面 sweep 成本过高且不利于先定位收益来源

### 5. resident 模式只让 compressed plane 常驻，raw/payload 仍保持磁盘路径

选择：
- preload 只覆盖 `.clu` 中的 compressed plane 与地址元数据
- raw vectors / payload 仍通过现有路径按 query 读取

原因：
- 这才与当前 baseline 的 warm resident compressed-plane 假设对齐
- 否则会把问题变成“全库全内存”而不是“compressed search resident”

备选：
- 同时 preload raw vector plane
- 否决原因：会掩盖 compressed-plane resident 化本身的收益

## Risks / Trade-offs

- [Resident cluster 结构体如果继续复制 `decoded_addresses`] → 必须把 probe 路径改成直接消费全局 resident 结构，避免 query-local 拷贝
- [full_preload 前移后，旧测试默认依赖首次查询隐式 preload] → benchmark 和 warm-up 流程中显式加 resident prepare 步骤，并在输出中写明是否已 preload
- [Resident 模式显著改善小数据集 warm latency，但对更大数据集收益有限] → 第一轮先固定 `coco_100k` 做 before/after，若收益明确再扩展到更大数据集
- [全局 resident 结构增大内存占用] → benchmark 必须导出 `resident_cluster_mem_bytes`，把内存换时间的代价显式化
- [保留现有 CRC / Stage 2 语义后，查询仍可能受 CPU 控制流主导] → 本次 change 先只剥离 cluster-load/path 成本，后续再单独判断是否需要继续优化 CPU probe 核

## Migration Plan

1. 在存储层把 resident preload 结构收束为全局 probe-ready cluster view
2. 在查询层把 `full_preload` 热路径改成直接消费全局 resident 结构
3. 将 preload 触发点从 `Search()` 前移到 benchmark / warm-up 阶段
4. 在 `bench_e2e` 中增加 resident mode 前后对照输出与 resident 指标
5. 先在 `coco_100k` 上做 before/after 验证
6. 若收益显著，再将 resident mode 纳入后续 warm Pareto 实验

回滚策略：
- 配置切回旧 `full_preload` 路径
- 保留现有 `window` 和旧 resident 路径，不做破坏性删除

## Open Questions

- probe 路径是直接改成消费 `ResidentClusterView`，还是新增 `ResidentParsedCluster` 类型更清晰？
- `resident_cluster_mem_bytes` 是否只统计 `.clu` resident buffer，还是要把全局 cluster-view 容器的附加开销一起计入？
- `bench_e2e` 的 before/after 对照是否需要额外导出 `parse_cluster_ms` / `uring_submit_ms` 的变化，用来直接证明收益来源？
