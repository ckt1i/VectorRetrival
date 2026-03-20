# Design: Async Cluster Prefetch (Phase 8)

## 1. 架构总览

```
SearchRequest(query_vec, top_k, nprobe)
  │
  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  OverlapScheduler — 单线程，统一事件循环                                    │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐│
│  │  Step 0: PrefetchClusters                                           ││
│  │    FindNearestClusters → sorted_clusters[0..nprobe)                 ││
│  │    批量提交 min(prefetch_depth, nprobe) 个 cluster block reads        ││
│  │    → io_uring Submit                                                ││
│  └────────────────────────────────────┬────────────────────────────────┘│
│                                       ▼                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐│
│  │  Step 1: ProbeAndDrainInterleaved (统一事件循环)                      ││
│  │                                                                     ││
│  │  for i = 0..nprobe:                                                 ││
│  │    cid = sorted_clusters[i]                                         ││
│  │    while cid not in ready_clusters_:                                ││
│  │      WaitAndPoll → dispatch CQEs:                                   ││
│  │        CLUSTER_BLOCK → ParseClusterBlock → ready_clusters_          ││
│  │        VEC_ONLY/ALL  → ConsumeVec/ConsumeAll (rerank)               ││
│  │        PAYLOAD       → ConsumePayload (cache)                       ││
│  │                                                                     ││
│  │    probe(cid, ready_clusters_[cid]):                                ││
│  │      RaBitQ EstimateBatch → ConANN Classify                         ││
│  │      → PrepRead 原始向量 → Submit                                    ││
│  │    ready_clusters_.erase(cid)  // probe 完释放                       ││
│  │                                                                     ││
│  │    // 滑动窗口补充                                                    ││
│  │    if inflight_clusters < refill_threshold:                         ││
│  │      补充提交 refill_count 个 cluster reads → Submit                 ││
│  └────────────────────────────────────┬────────────────────────────────┘│
│                                       ▼                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐│
│  │  Step 2: FinalDrain                                                 ││
│  │    drain 所有剩余 VEC/ALL/PAYLOAD CQEs                               ││
│  └────────────────────────────────────┬────────────────────────────────┘│
│                                       ▼                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐│
│  │  Step 3: FetchMissingPayloads (不变)                                 ││
│  └────────────────────────────────────┬────────────────────────────────┘│
│                                       ▼                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐│
│  │  Step 4: AssembleResults (不变)                                      ││
│  └─────────────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────────┘
```

**时序图**（nprobe=8, prefetch_depth=4）：

```
时间 ─────────────────────────────────────────────────────────────────────▶

io_uring:  ┌── submit c0,c1,c2,c3 block reads ──┐
           └──┬──────┬──────┬──────┬─────────────┘
              ▼      ▼      ▼      ▼
CPU:     [wait c0][probe c0][wait c1...顺带 rerank c0 vecs][probe c1]
                  │submit    │                              │submit
                  │vec reads │rerank c0 vec CQEs            │vec reads
                  ▼          ▼                              ▼
         inflight_clu=3     inflight_clu=2
                            inflight_clu=1 < 2 → submit c4,c5
                                                              ...

关键: cluster block 读取、probe CPU 计算、原始向量 rerank 三者流水线化
      严格按 c0→c1→c2...距离顺序 probe，但等待过程中消化其他 CQE
```

## 2. 组件详细设计

### 2.1 ParsedCluster — 独立的解析后 cluster 数据

查询路径完全绕过 `loaded_clusters_`，使用独立的 `ParsedCluster` 结构：

```cpp
/// 异步读取 + 解析后的 cluster 数据，独立于 ClusterStoreReader::ClusterData。
/// 生命周期由 OverlapScheduler::ready_clusters_ 管理，probe 完即释放。
struct ParsedCluster {
    std::unique_ptr<uint8_t[]> block_buf;  // 持有整个 block buffer

    // codes 零拷贝：直接指向 block_buf 内部
    const uint8_t* codes_start = nullptr;
    uint32_t code_entry_size = 0;          // bytes per code entry

    // address 需要 SIMD 解码，独立存储
    uint32_t num_records = 0;
    std::vector<AddressEntry> decoded_addresses;
};
```

**为什么不复用 `ClusterStoreReader::ClusterData`**：
- `ClusterData` 是 ClusterStoreReader 的 private 成员，改成 public 侵入性大
- 查询路径不需要 `address_layout`、`address_blocks` 等中间解析状态
- `ParsedCluster` 更精简，只保留 probe 需要的字段
- 两套结构互不干扰：`EnsureClusterLoaded` 路径完全不改

**零拷贝说明**：整个 cluster block 通过 io_uring 一次读入 `block_buf`，`codes_start = block_buf.get()` 直接指向 buffer 起始（codes region 在 block 开头），无需拷贝 codes region。

### 2.2 ClusterStoreReader 最小改动

只新增 3 个接口，不修改任何现有接口或内部结构：

```cpp
class ClusterStoreReader {
public:
    // ... 所有现有接口保持不变 ...

    /// 获取 .clu 文件描述符（用于 io_uring PrepRead）
    int clu_fd() const { return fd_; }

    /// 查询 cluster block 位置（纯内存，从 lookup_table 查）
    /// 返回 nullopt 如果 cluster_id 不存在
    std::optional<ClusterBlockLocation> GetBlockLocation(uint32_t cluster_id) const;

    /// 从已读入内存的 raw block buffer 解析出 ParsedCluster
    /// 纯 CPU 操作，不做任何 I/O
    /// block_buf 的所有权转移给 out
    Status ParseClusterBlock(uint32_t cluster_id,
                              std::unique_ptr<uint8_t[]> block_buf,
                              uint64_t block_size,
                              ParsedCluster& out);
};
```

**不新增**：`InjectClusterData`、`IsClusterLoaded`（查询路径不碰 `loaded_clusters_`）

**不修改**：`ClusterData`、`EnsureClusterLoaded`、`GetCodePtr`、`GetAddress`（全部保持原样）

#### ClusterBlockLocation

```cpp
struct ClusterBlockLocation {
    uint64_t offset;       // block_offset in .clu file
    uint64_t size;         // block_size in bytes
    uint32_t num_records;  // 该 cluster 的记录数
};
```

#### ParseClusterBlock 伪逻辑

```
ParseClusterBlock(cluster_id, block_buf, block_size, &out):
  out.block_buf = std::move(block_buf)

  // 1. 从 block 末尾解析 mini-trailer（与 EnsureClusterLoaded 相同）
  trailer = parse_mini_trailer(out.block_buf.get(), block_size)

  // 2. codes 零拷贝
  out.codes_start = out.block_buf.get()  // codes 在 block 开头
  out.code_entry_size = num_code_words() * sizeof(uint64_t)

  // 3. address 解码
  out.num_records = info_.lookup_table[cluster_index_[cluster_id]].num_records
  // 解析 address blocks → SIMD prefix_sum decode → decoded_addresses
  out.decoded_addresses = decode_addresses(trailer, out.block_buf.get(), block_size)

  return OK
```

### 2.3 Segment 转发

```cpp
class Segment {
public:
    // ... 现有转发保持不变 ...

    int clu_fd() const { return clu_reader_.clu_fd(); }

    std::optional<ClusterBlockLocation> GetBlockLocation(uint32_t cluster_id) const {
        return clu_reader_.GetBlockLocation(cluster_id);
    }

    Status ParseClusterBlock(uint32_t cluster_id,
                              std::unique_ptr<uint8_t[]> block_buf,
                              uint64_t block_size,
                              ParsedCluster& out) {
        return clu_reader_.ParseClusterBlock(cluster_id,
                                              std::move(block_buf), block_size, out);
    }
};
```

### 2.4 OverlapScheduler 改造

#### 2.4.1 PendingIO — 统一的 in-flight I/O 元信息

```cpp
struct PendingIO {
    enum class Type : uint8_t {
        CLUSTER_BLOCK,   // .clu 文件的 cluster block 读取
        VEC_ONLY,        // .dat 文件的原始向量读取
        VEC_ALL,         // .dat 文件的全量读取 (vec + payload)
        PAYLOAD,         // .dat 文件的 payload 读取
    };

    Type type;
    uint32_t cluster_id;     // CLUSTER_BLOCK 用
    uint32_t block_size;     // CLUSTER_BLOCK 用
    AddressEntry addr;       // VEC_ONLY / VEC_ALL / PAYLOAD 用
    uint64_t read_offset;    // VEC_ONLY / VEC_ALL / PAYLOAD 用
    uint32_t read_length;    // VEC_ONLY / VEC_ALL / PAYLOAD 用
};
```

与之前的 `PendingRead` + `InferType` 方案不同，新方案在提交时就明确标记类型，无需从参数推断。这更清晰，因为现在有两个 fd 来源（.clu 和 .dat），InferType 无法区分。

#### 2.4.2 OverlapScheduler 成员

```cpp
class OverlapScheduler {
public:
    OverlapScheduler(index::IvfIndex& index, AsyncReader& reader,
                     const SearchConfig& config);
    ~OverlapScheduler();

    SearchResults Search(const float* query_vec);

private:
    // ── 新增阶段 ──
    void PrefetchClusters(SearchContext& ctx,
                          const std::vector<uint32_t>& sorted_clusters);
    void ProbeAndDrainInterleaved(SearchContext& ctx,
                                   RerankConsumer& reranker,
                                   const std::vector<uint32_t>& sorted_clusters);
    void FinalDrain(SearchContext& ctx, RerankConsumer& reranker);

    // ── 保留阶段 ──
    void FetchMissingPayloads(SearchContext& ctx,
                              RerankConsumer& reranker,
                              const std::vector<CollectorEntry>& results);
    SearchResults AssembleResults(RerankConsumer& reranker,
                                  const std::vector<CollectorEntry>& results);

    // ── CQE 分发 ──
    void DispatchCompletion(uint8_t* buf, SearchContext& ctx,
                            RerankConsumer& reranker);

    // ── cluster 提交辅助 ──
    void SubmitClusterRead(uint32_t cluster_id);

    // ── Probe 单个 cluster（直接操作 ParsedCluster）──
    void ProbeCluster(const ParsedCluster& cluster, uint32_t cluster_id,
                      SearchContext& ctx, RerankConsumer& reranker);

    index::IvfIndex& index_;
    AsyncReader& reader_;
    SearchConfig config_;
    BufferPool buffer_pool_;

    // 统一的 in-flight map
    std::unordered_map<uint8_t*, PendingIO> pending_;

    // 已就绪但未 probe 的 cluster 数据（乱序完成 → 缓冲）
    // probe 完 → erase → 释放内存
    // 不碰 loaded_clusters_
    std::unordered_map<uint32_t, ParsedCluster> ready_clusters_;

    // 滑动窗口状态
    uint32_t next_to_submit_ = 0;    // 下一个要提交读取的 cluster 索引
    uint32_t inflight_clusters_ = 0; // 当前 in-flight 的 cluster block 读取数

    uint32_t vec_bytes_;
    uint32_t num_words_;
};
```

#### 2.4.3 SearchConfig 扩展

```cpp
struct SearchConfig {
    // ... existing fields ...
    uint32_t prefetch_depth = 16;       // 初始预取的 cluster 数量
    uint32_t refill_threshold = 2;      // inflight cluster 数低于此值时补充
    uint32_t refill_count = 2;          // 每次补充的 cluster 数量
};
```

### 2.5 Search 主流程

```
Search(query_vec):
  ctx = SearchContext(query_vec, config)
  reranker = RerankConsumer(ctx, dim)
  sorted_clusters = FindNearestClusters(query_vec, nprobe)

  // ═══ Step 0: 预取 ═══
  PrefetchClusters(ctx, sorted_clusters)

  // ═══ Step 1: 统一事件循环 ═══
  ProbeAndDrainInterleaved(ctx, reranker, sorted_clusters)

  // ═══ Step 2: 消化剩余 CQE ═══
  FinalDrain(ctx, reranker)

  results = ctx.collector().Finalize()

  // ═══ Step 3: 补读 payload (不变) ═══
  FetchMissingPayloads(ctx, reranker, results)

  // ═══ Step 4: 组装 (不变) ═══
  return AssembleResults(reranker, results)
```

### 2.6 PrefetchClusters

```
PrefetchClusters(ctx, sorted_clusters):
  clu_fd = index_.segment().clu_fd()
  initial = min(config_.prefetch_depth, sorted_clusters.size())

  for i = 0..initial:
    cid = sorted_clusters[i]
    loc = index_.segment().GetBlockLocation(cid)
    buf = buffer_pool_.Acquire(loc.size)
    reader_.PrepRead(clu_fd, buf, loc.size, loc.offset)
    pending_[buf] = PendingIO{CLUSTER_BLOCK, cid, loc.size, ...}
    inflight_clusters_++

  next_to_submit_ = initial
  submitted = reader_.Submit()
  ctx.stats().total_io_submitted += submitted
```

### 2.7 ProbeAndDrainInterleaved

```
ProbeAndDrainInterleaved(ctx, reranker, sorted_clusters):
  comps[128]

  for i = 0..nprobe:
    cid = sorted_clusters[i]

    // 1. 等待 cid 就绪
    while cid not in ready_clusters_:
      n = reader_.WaitAndPoll(comps, 128)
      for each comp:
        DispatchCompletion(comp.buffer, ctx, reranker)

    // 2. 直接从 ready_clusters_ 取数据 probe
    ProbeCluster(ready_clusters_[cid], cid, ctx, reranker)

    // 3. probe 完释放 cluster 内存
    ready_clusters_.erase(cid)

    // 4. Submit 本 cluster 的原始向量读取
    submitted = reader_.Submit()
    ctx.stats().total_io_submitted += submitted

    // 5. 滑动窗口补充
    if inflight_clusters_ < config_.refill_threshold:
      refill = min(config_.refill_count,
                   sorted_clusters.size() - next_to_submit_)
      for j = 0..refill:
        cid_next = sorted_clusters[next_to_submit_]
        SubmitClusterRead(cid_next)
        next_to_submit_++
      if refill > 0:
        reader_.Submit()
```

### 2.8 DispatchCompletion — CQE 类型分发

```
DispatchCompletion(buf, ctx, reranker):
  it = pending_.find(buf)
  if it == end: return
  io = it->second
  pending_.erase(it)

  switch io.type:
    case CLUSTER_BLOCK:
      inflight_clusters_--
      // 解析 block buffer → ParsedCluster
      block_buf = unique_ptr(buf)  // 所有权转移
      ParsedCluster pc
      index_.segment().ParseClusterBlock(
          io.cluster_id, std::move(block_buf), io.block_size, pc)
      ready_clusters_[io.cluster_id] = std::move(pc)
      // 注意: 不 Release buffer（所有权已转给 ParsedCluster.block_buf）

    case VEC_ONLY:
      reranker.ConsumeVec(buf, io.addr)
      buffer_pool_.Release(buf)

    case VEC_ALL:
      reranker.ConsumeAll(buf, io.addr)
      buffer_pool_.Release(buf)

    case PAYLOAD:
      reranker.ConsumePayload(buf, io.addr)
      // 所有权转移，不 Release
```

**CLUSTER_BLOCK buffer 生命周期**：io_uring 读取的 buffer 所有权通过 `unique_ptr` 转移给 `ParsedCluster.block_buf`，`codes_start` 指向该 buffer 内部。probe 完后 `ready_clusters_.erase(cid)` → `ParsedCluster` 析构 → `block_buf` 释放。

### 2.9 FinalDrain

```
FinalDrain(ctx, reranker):
  comps[128]
  while !pending_.empty():
    n = reader_.WaitAndPoll(comps, 128)
    for each comp:
      DispatchCompletion(comp.buffer, ctx, reranker)
```

### 2.10 ProbeCluster — 直接操作 ParsedCluster

不经过 `GetCodePtr`/`GetAddress`，直接从 `ParsedCluster` 取数据：

```
ProbeCluster(const ParsedCluster& pc, cid, ctx, reranker):
  n_records = pc.num_records
  centroid = index_.centroid(cid)
  pq = estimator.PrepareQuery(query_vec, centroid, rotation)
  dat_fd = index_.segment().data_reader().fd()

  for offset = 0..n_records step batch_size:
    actual = min(batch_size, n_records - offset)

    // Build RaBitQCode batch — 直接从 ParsedCluster 取
    for i = 0..actual:
      words = (uint64_t*)(pc.codes_start + (offset + i) * pc.code_entry_size)
      batch_codes[i].code.assign(words, words + num_words_)
      batch_codes[i].norm = 0.0f
      batch_codes[i].sum_x = PopcountTotal(words, num_words_)

    EstimateDistanceBatch(pq, batch_codes, actual, dists)

    for i = 0..actual:
      rc = conann.Classify(dists[i])
      addr = pc.decoded_addresses[offset + i]  // 直接从 ParsedCluster 取

      if rc == SafeOut: continue
      if rc == SafeIn:
        // PrepRead ALL/VEC_ONLY, pending_ 标记为 VEC_ALL/VEC_ONLY
      else: // Uncertain
        // PrepRead VEC_ONLY
```

**与旧 ProbeAndSubmit 的 3 处差异**：
1. ❌ `EnsureClusterLoaded(cid)` → 不需要
2. ❌ `segment.GetCodePtr(cid, idx)` → `pc.codes_start + idx * pc.code_entry_size`
3. ❌ `segment.GetAddress(cid, idx)` → `pc.decoded_addresses[idx]`

## 3. 数据流详图

```
═══════════════ Step 0: PrefetchClusters ═══════════════

  FindNearestClusters → [c0, c1, ..., c31] (按距离排序)

  Submit cluster block reads for c0..c15:
  ┌──────────────────────────────────────────────────────────┐
  │  for i = 0..15:                                          │
  │    loc = GetBlockLocation(sorted[i])                     │
  │    buf = pool.Acquire(loc.size)                          │
  │    reader.PrepRead(clu_fd, buf, loc.size, loc.offset)    │
  │    pending_[buf] = {CLUSTER_BLOCK, cluster_id, ...}      │
  │  reader.Submit()                                         │
  └──────────────────────────────────────────────────────────┘


═══════════════ Step 1: ProbeAndDrainInterleaved ═══════════════

  ┌──────────────────────────────────────────────────────────┐
  │  for i = 0..nprobe:                                      │
  │    cid = sorted[i]                                       │
  │                                                          │
  │    // Wait for cid to be ready                           │
  │    while cid not in ready_clusters_:                     │
  │      WaitAndPoll → DispatchCompletion:                   │
  │        CLUSTER_BLOCK → Parse → ready_clusters_           │
  │        VEC_ONLY/ALL  → Rerank (消化前序 cluster 的 vec)  │
  │        PAYLOAD       → Cache                             │
  │                                                          │
  │    // Probe directly from ParsedCluster                  │
  │    ProbeCluster(ready_clusters_[cid], cid)               │
  │    ready_clusters_.erase(cid)  // 释放 cluster 内存      │
  │    Submit()                                              │
  │                                                          │
  │    // Refill check                                       │
  │    if inflight_clusters_ < 2:                            │
  │      submit 2 more cluster block reads                   │
  └──────────────────────────────────────────────────────────┘


═══════════════ Step 2: FinalDrain ═══════════════

  while pending_ not empty:
    WaitAndPoll → DispatchCompletion
    (消化最后一批 cluster 的 vec CQEs)


═══════════════ Step 3 & 4: 不变 ═══════════════
  FetchMissingPayloads → AssembleResults
```

## 4. Buffer 生命周期

```
┌───────────────────────────────────────────────────────────────┐
│  CLUSTER_BLOCK buffer:                                        │
│    Step 0: pool.Acquire(block_size) → PrepRead(.clu) → Submit│
│    Step 1: CQE → ParseClusterBlock → ParsedCluster.block_buf │
│            → codes_start 零拷贝指向 block_buf 内部            │
│    probe 期间: ProbeCluster 读取 codes_start                  │
│    probe 完: ready_clusters_.erase(cid)                       │
│              → ParsedCluster 析构 → block_buf 释放            │
│    注意: 不走 pool.Release!                                   │
│                                                               │
│  VEC_ONLY / VEC_ALL / PAYLOAD buffer:                        │
│    与 Phase 7 完全相同，不变                                   │
│                                                               │
│  ready_clusters_ 生命周期:                                    │
│    CQE 完成 → ready_clusters_[cid] = ParsedCluster           │
│    probe 该 cluster 时 → ProbeCluster 直接引用                │
│    probe 完 → erase → ParsedCluster 析构                     │
│    查询结束时 ready_clusters_ 应为空                           │
│                                                               │
│  loaded_clusters_ 完全不参与查询路径                           │
└───────────────────────────────────────────────────────────────┘
```

## 5. 已确定决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | Probe 顺序 | 严格按距离排序 | 最大化 ConANN SafeOut 剪枝，减少无效 I/O |
| 2 | 预取策略 | 滑动窗口 (prefetch=16, threshold=2, refill=2) | 初始批量打满 I/O 带宽，后续按需补充 |
| 3 | Cluster block 读取方式 | 整块一次 io_uring read | 利用 lookup_table 已有的 offset+size，避免多次 pread 的依赖链 |
| 4 | Codes 存储 | 零拷贝指针指向 block_buf | 避免 codes region 的内存拷贝 |
| 5 | PendingIO 类型 | 显式标记 (CLUSTER_BLOCK/VEC_ONLY/VEC_ALL/PAYLOAD) | 两个 fd 来源无法用 InferType 区分 |
| 6 | 事件循环模型 | 统一循环，等待特定 cluster + 顺带消化其他 CQE | 最大化 CPU/IO 重叠 |
| 7 | CLUSTER_BLOCK buffer 回收 | 不走 BufferPool，所有权转给 ParsedCluster | buffer 需在 probe 期间存活，probe 完释放 |
| 8 | Cluster 数据结构 | **独立 ParsedCluster（方案 B）** | 不碰 ClusterStoreReader::ClusterData，不侵入 loaded_clusters_，ProbeCluster 直接操作 ParsedCluster |
| 9 | loaded_clusters_ | **查询路径完全不参与** | 不检查、不写入、不注入。简化设计，单查询场景无需跨查询缓存 |
| 10 | EnsureClusterLoaded | **不修改** | 保持原有同步路径不变，新路径完全独立 |

## 6. 未来优化方向

| 优化 | 描述 | 预期收益 |
|------|------|----------|
| loaded_clusters_ 集成 | 查询完成后将 ParsedCluster 转化注入 loaded_clusters_ | 跨查询复用热 cluster |
| 全量预加载 | Open() 时异步加载所有 cluster | 稳态下零 cluster I/O |
| 乱序 probe | 按 CQE 完成顺序 probe（放弃严格距离序） | 减少 CPU 空转等待 |
| 自适应 prefetch_depth | 根据 cluster block 平均大小和 I/O 带宽自动计算 | 适配不同硬件 |
