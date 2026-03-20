# Spec: Async Cluster Prefetch

## Overview

将聚类数据加载从同步 pread 改为 io_uring 异步预取，通过滑动窗口机制与现有查询流水线的原始向量读取共享同一 io_uring ring，实现 cluster I/O、probe CPU 计算、rerank 三者的全程流水线化。

## Requirements

### R1: 独立 ParsedCluster 结构
- 新建 `ParsedCluster` struct，独立于 `ClusterStoreReader::ClusterData`
- `block_buf (unique_ptr<uint8_t[]>)` 持有整个 cluster block buffer 的所有权
- `codes_start (const uint8_t*)` 零拷贝指向 `block_buf` 内部 codes 区域
- `code_entry_size (uint32_t)` 每条 code 的字节数
- `num_records (uint32_t)` 该 cluster 的记录数
- `decoded_addresses (vector<AddressEntry>)` SIMD 解码后的地址表
- Move-only，禁止拷贝

### R2: ClusterStoreReader 最小扩展
- `clu_fd()` 返回 .clu 文件描述符
- `GetBlockLocation(cluster_id)` 从 lookup_table 查询 cluster block 的文件偏移和大小，纯内存查询，返回 `optional<ClusterBlockLocation>`
- `ParseClusterBlock(cluster_id, block_buf, block_size, &out)` 从 raw buffer 解析出 `ParsedCluster`，纯 CPU 操作，不做任何 I/O，block_buf 所有权转移给 out
- **不修改**任何现有接口：ClusterData、EnsureClusterLoaded、GetCodePtr、GetAddress、loaded_clusters_ 全部保持原样
- Segment 转发以上 3 个新接口

### R3: 滑动窗口预取
- 查询开始时批量提交 `prefetch_depth`（默认 16）个 cluster block 读取
- 每 probe 完一个 cluster 后检查 `inflight_clusters_`，低于 `refill_threshold`（默认 2）时补充 `refill_count`（默认 2）个
- `prefetch_depth`、`refill_threshold`、`refill_count` 可通过 SearchConfig 配置

### R4: 严格距离顺序 probe
- 始终按 FindNearestClusters 返回的距离排序顺序 probe cluster
- io_uring CQE 完成顺序可能乱序，通过 `ready_clusters_` map 缓冲
- 在等待特定 cluster 就绪的 WaitAndPoll 循环中，同时消化其他类型的 CQE（VEC_ONLY/VEC_ALL/PAYLOAD 的 rerank）

### R5: ProbeCluster 直接操作 ParsedCluster
- ProbeCluster 接受 `const ParsedCluster&` 参数
- 从 `pc.codes_start + idx * pc.code_entry_size` 取 code（替代 GetCodePtr）
- 从 `pc.decoded_addresses[idx]` 取 address（替代 GetAddress）
- 不调用 EnsureClusterLoaded、GetCodePtr、GetAddress
- `loaded_clusters_` 完全不参与查询路径
- probe 完后 `ready_clusters_.erase(cid)` 释放 ParsedCluster 内存

### R6: 统一事件循环
- cluster block 读取（.clu fd）和原始向量读取（.dat fd）共享同一个 AsyncReader / io_uring ring
- CQE 通过 `PendingIO.type` 区分处理路径：CLUSTER_BLOCK / VEC_ONLY / VEC_ALL / PAYLOAD
- 不使用 InferType 推断（两个 fd 来源无法区分），提交时显式标记类型

### R7: CLUSTER_BLOCK buffer 生命周期
- 由 BufferPool.Acquire 分配
- CQE 完成后所有权转移到 ParsedCluster.block_buf，不通过 BufferPool.Release 回收
- probe 完后 ready_clusters_.erase → ParsedCluster 析构 → block_buf 释放
- 查询结束时 ready_clusters_ 应为空

## Constraints

- 单线程执行，无锁
- 与现有 PreadFallbackReader 兼容（fallback 路径同样受益于整块读取 + ParsedCluster）
- 不改变 AsyncReader 接口（PrepRead 已支持任意 fd）
- 不改变 RerankConsumer、ResultCollector、SearchResults 接口
- 不修改 ClusterStoreReader 的现有接口和内部结构
