# Design: Query Pipeline (Phase 7)

> 权衡分析详见 [design-tradeoffs.md](./design-tradeoffs.md)

## 1. 架构总览

```
SearchRequest(query_vec, top_k, nprobe)
  │
  ▼
┌────────────────────────────────────────────────────────────────────┐
│  OverlapScheduler — 单线程两阶段                                    │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 1: Probe + Submit                                    │   │
│  │                                                             │   │
│  │  for each cluster:                                          │   │
│  │    EnsureClusterLoaded → PrepareQuery                       │   │
│  │    EstimateBatch (mini-batch 64) → Classify                 │   │
│  │    non-SafeOut → PrepRead × N → Submit (flush per cluster)  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 2: Drain + Rerank                                    │   │
│  │                                                             │   │
│  │  while pending_ not empty:                                  │   │
│  │    WaitAndPoll → InferType → ConsumeVec/All/Payload         │   │
│  │    L2Sqr → TryInsert TopK                                   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 3: 补读 Uncertain payload                             │   │
│  │  TopK 中 payload=nullptr 的条目 → 批量 PAYLOAD 读取            │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 4: 组装 SearchResults (RAII)                          │   │
│  │  从 payload_cache 查找 → 填入指针 → 返回                       │   │
│  └─────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────┘
```

**执行模型**：单线程、两阶段顺序执行。Phase 1 全速 Probe 所有 cluster 并提交 I/O 到
io_uring SQ；Phase 2 从 io_uring CQ 逐个读取完成结果并 Rerank。CPU/IO 重叠来自
内核——Phase 1 期间内核已开始处理 SQ 中的请求，Phase 2 开始时部分结果已就绪。

**无应用层队列**：io_uring 的 SQ/CQ 即为唯一的任务队列，不再需要 ReadQueue、
SPSC queue 或任何中间队列。

**时序图**：

```
假设 nprobe=4, 每cluster ~200条, dim=128

时间 ─────────────────────────────────────────────────────────────────►

CPU      [Probe C0][Probe C1][Probe C2][Probe C3]  [Rerank: drain CQ]
          ↓Submit   ↓Submit   ↓Submit   ↓Submit     ↑WaitAndPoll
          │         │         │         │           │
io_uring  ·····[C0 reads processing][C1 reads][C2+C3 reads]···[done]
(kernel)  ─── SQ entries accumulate, kernel processes in background ──

关键:
  Phase 1: Probe 全速，每个 cluster probe 完毕就 Submit flush
           内核开始执行 I/O，与后续 cluster 的 Probe CPU 计算重叠
  Phase 2: 所有 Probe 完成后，开始 drain CQ
           大部分 I/O 已由内核完成，CQ 中已有大量就绪结果
           L2Sqr 精排穿插 WaitAndPoll

  vs 交错模型 (interleaved): TopK 阈值不能实时更新
  → ~10-20% 额外 I/O 请求 (nprobe=8)，可接受
  代价: 简单性、无队列、无线程同步
```

## 2. 组件详细设计

### 2.1 AsyncReader — I/O 后端接口

薄封装设计（权衡 13：方案 A），调用方管理 fd 和 buffer。
接口映射 io_uring 原语：`PrepRead` → `io_uring_prep_read`，`Submit` → `io_uring_submit`。

```cpp
namespace vdb::query {

/// I/O 完成结果
struct IoCompletion {
    uint8_t* buffer;       // 从 CQE user_data 还原的 buffer 指针
    int32_t result;        // 成功: 读取字节数, 失败: 负 errno
};

class AsyncReader {
public:
    virtual ~AsyncReader() = default;

    /// 准备一次读取（填 SQE，不提交）
    /// 对应 io_uring_prep_read + io_uring_sqe_set_data
    virtual Status PrepRead(int fd, uint8_t* buf,
                            uint32_t len, uint64_t offset) = 0;

    /// 提交所有已准备的 SQE
    /// 对应 io_uring_submit
    /// @return 本次提交的 SQE 数量
    virtual uint32_t Submit() = 0;

    /// 轮询已完成请求（非阻塞）
    virtual uint32_t Poll(IoCompletion* out, uint32_t max_count) = 0;

    /// 等待至少一个完成（阻塞）
    virtual uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) = 0;

    /// 当前在途请求数
    virtual uint32_t InFlight() const = 0;
};

}
```

**构建依赖**：liburing 为必须依赖，CMake `find_library(uring)` 失败则报错终止。

**IoUringReader**（主实现）：
- `Init(queue_depth=64, cq_entries=4096)` → `io_uring_queue_init_params`
  - `IORING_SETUP_CQSIZE` 设置 CQ 容量为 4096，防止大 nprobe 场景 CQ 溢出
- `PrepRead` → `io_uring_get_sqe` + `io_uring_prep_read` + `io_uring_sqe_set_data(sqe, buf)`
- `Submit` → `io_uring_submit`，返回提交数量
- `Poll` → `io_uring_peek_cqe` 循环
- `WaitAndPoll` → `io_uring_wait_cqe` + 消费所有就绪的 CQE

**PreadFallbackReader**（运行时降级）：
- 当 io_uring 初始化失败时（如内核不支持），打印警告并自动降级到此实现
- `PrepRead` → 记录到内部 pending list
- `Submit` → 同步执行所有 pending 的 `pread` → 结果存入内部完成队列
- `Poll` → 从内部队列返回
- `InFlight` → 始终返回 0（同步完成）

### 2.2 ReadTaskType

```cpp
enum class ReadTaskType : uint8_t {
    VEC_ONLY,   // 只读向量部分 (dim * sizeof(float) 字节)
    ALL,        // 全量读取 (vec + payload, 用于小 record 的 SafeIn)
    PAYLOAD,    // 只读 payload 部分 (从 offset + dim*4 开始)
};
```

**SafeIn 全量读取阈值**：256KB (`safein_all_threshold`)。
- record_size ≤ 256KB 时，NVMe 读取延迟与 VEC_ONLY (512B) 在同一量级（~25μs），且省一半 IOPS 和 SQE 槽位
- record_size > 256KB 时，ALL 占用 SQE 槽位过久（>50μs），拆分为 VEC_ONLY + PAYLOAD 避免阻塞

**分类与 I/O 提交规则**：

```
Phase 1 Probe 产出:
  SafeOut    → skip

  SafeIn + addr.size ≤ 256KB:
             → PrepRead ALL{fd, buf, addr.size, addr.offset}
               (一次 I/O 读完 vec + payload)

  SafeIn + addr.size > 256KB:
             → PrepRead VEC_ONLY{fd, buf, dim*4, addr.offset}
               PrepRead PAYLOAD{fd, buf, payload_len, addr.offset + dim*4}
               (拆分：向量快速到达 Rerank, payload 分开读取)

  Uncertain  → PrepRead VEC_ONLY{fd, buf, dim*4, addr.offset}

Phase 3 补读:
  TopK 中 payload 不在 cache 的条目 (Uncertain)
             → PrepRead PAYLOAD{fd, buf, payload_len, addr.offset + dim*4}
```

### 2.3 ResultCollector — Top-K 收集

```cpp
/// 独立 struct（非嵌套），RerankConsumer 和 OverlapScheduler 均引用
struct CollectorEntry {
    float distance;
    AddressEntry addr;
    bool operator<(const CollectorEntry& o) const {
        return distance < o.distance;  // max-heap
    }
};

class ResultCollector {
public:
    explicit ResultCollector(uint32_t top_k);

    /// 尝试插入。若 dist < 当前堆顶（或堆未满），插入并返回 true
    bool TryInsert(float distance, AddressEntry addr);

    /// 当前堆顶距离（最差的 top-k 结果）
    float TopDistance() const;

    /// 堆是否已满
    bool Full() const;

    /// 堆大小
    uint32_t Size() const;

    /// 排序输出最终结果（distance 升序）
    std::vector<CollectorEntry> Finalize();

private:
    uint32_t top_k_;
    // max-heap: 堆顶是最大距离
    std::priority_queue<CollectorEntry> heap_;
};
```

**无 VecID**：堆只存 `(distance, AddressEntry)`。不返回 ID，最终结果只包含距离和原始数据。

### 2.4 SearchResult 与 SearchResults (RAII)

```cpp
/// 单条搜索结果
struct SearchResult {
    float distance;     // Rerank 后的 L2 精确距离
    Datum* payload;     // 指向 owned_buffers_ 中的解析数据（单个 Datum 值）
};
// 注: 初期 payload 为单个 Datum（非多列 Datum 序列），后续可扩展为多列

/// RAII 包装，管理所有 payload buffer 的生命周期
class SearchResults {
public:
    ~SearchResults();   // 自动释放所有 owned_buffers_

    // 迭代器接口
    auto begin() { return results_.begin(); }
    auto end() { return results_.end(); }
    auto begin() const { return results_.begin(); }
    auto end() const { return results_.end(); }
    size_t size() const { return results_.size(); }
    const SearchResult& operator[](size_t i) const;

    // 禁止拷贝，允许移动
    SearchResults(SearchResults&&) = default;
    SearchResults& operator=(SearchResults&&) = default;
    SearchResults(const SearchResults&) = delete;
    SearchResults& operator=(const SearchResults&) = delete;

private:
    friend class OverlapScheduler;  // 只有 Scheduler 能构造
    std::vector<SearchResult> results_;
    std::vector<std::unique_ptr<uint8_t[]>> owned_buffers_;
    // payload_cache 中的 buffer 所有权转移到这里
};
```

**生命周期**：用户拿到 `SearchResults` 后，`payload` 指针在 `SearchResults` 存活期间有效。离开作用域自动释放所有 buffer。

### 2.5 SearchContext — 查询上下文

```cpp
struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 8;
    uint32_t probe_batch_size = 64;              // mini-batch 大小（大聚类分批用）
    uint32_t cluster_atomic_threshold = 1024;    // ≤ 此值的聚类原子 probe
    uint32_t io_queue_depth = 64;                // io_uring SQ 深度
    uint32_t cq_entries = 4096;                  // io_uring CQ 容量
    uint32_t safein_all_threshold = 256 * 1024;  // SafeIn 全量读取上限 (256KB)
};

struct SearchStats {
    uint32_t total_probed = 0;
    uint32_t total_safe_in = 0;
    uint32_t total_safe_out = 0;
    uint32_t total_uncertain = 0;
    uint32_t total_io_submitted = 0;
    uint32_t total_reranked = 0;
    uint32_t total_payload_prefetched = 0;   // SafeIn payload 预读数
    uint32_t total_payload_fetched = 0;      // Phase 3 Uncertain payload 补读数
    double   probe_time_ms = 0;
    double   rerank_time_ms = 0;
    double   total_time_ms = 0;
};

class SearchContext {
public:
    SearchContext(const float* query_vec, const SearchConfig& config);

    const float* query_vec() const;
    const SearchConfig& config() const;
    SearchStats& stats();
    ResultCollector& collector();

private:
    const float* query_vec_;
    SearchConfig config_;
    SearchStats stats_;
    ResultCollector collector_;
};
```

### 2.6 RerankConsumer — Rerank + Payload Cache

```cpp
class RerankConsumer {
public:
    RerankConsumer(SearchContext& ctx, Dim dim);

    /// 处理一个 VEC_ONLY 完成项：L2Sqr 精排 → TryInsert
    /// buffer 用后由调用方释放
    void ConsumeVec(uint8_t* buffer, AddressEntry addr);

    /// 处理一个 ALL 完成项：L2Sqr 精排 → TryInsert → 进 TopK 则 memcpy payload 到新 buffer 入 cache
    /// 调用方始终负责释放原始 ALL buffer
    void ConsumeAll(uint8_t* buffer, AddressEntry addr);

    /// 处理一个 PAYLOAD 完成项：buffer 转移到 cache，调用方不释放
    void ConsumePayload(uint8_t* buffer, AddressEntry addr);

    /// 查询 payload cache 中是否有某地址的 payload
    bool HasPayload(uint64_t addr_offset) const;

    /// 从 cache 中取出 payload buffer（转移所有权）
    std::unique_ptr<uint8_t[]> TakePayload(uint64_t addr_offset);

    /// 将 buffer 存入 cache
    void CachePayload(uint64_t addr_offset, std::unique_ptr<uint8_t[]> buffer);

    /// 释放不在 TopK 中的 cache 条目
    void CleanupUnusedCache(const std::vector<CollectorEntry>& final_results);

private:
    SearchContext& ctx_;
    Dim dim_;
    // payload 缓存: addr.offset → buffer (payload 始终从 buf[0] 开始)
    // ALL 条目在 ConsumeAll 中已 memcpy 拆分, PAYLOAD 条目天然如此
    std::unordered_map<uint64_t, std::unique_ptr<uint8_t[]>> payload_cache_;
};
```

**ConsumeVec 逻辑**（Uncertain 的向量读取）：
1. 从 `buffer` 解析 raw vector（前 `dim * 4` 字节）
2. `L2Sqr(query_vec, parsed_vec, dim)` 计算精确距离
3. `collector.TryInsert(exact_dist, addr)`
4. 调用方释放 buffer

**ConsumeAll 逻辑**（SafeIn ≤256KB 的全量读取）：
1. 从 `buffer` 解析 raw vector（`buffer[0..dim*4]`）
2. `L2Sqr(query_vec, parsed_vec, dim)` 计算精确距离
3. `collector.TryInsert(exact_dist, addr)`
4. 若进入 TopK：
   - `memcpy` payload 部分（`buffer[dim*4..]`）到新分配的 payload buffer
   - `payload_cache_[addr.offset] = new_payload_buf`（payload 从 offset 0 开始）
5. **调用方始终释放原始 ALL buffer**（无论是否进入 TopK）

> 设计决策：ALL buffer 拆分而非保留整个 buffer。这确保 payload_cache 中所有条目
> 的 payload 均从 offset 0 开始，AssembleResults 无需区分 buffer 类型。
> 代价是一次 memcpy（≤256KB），但简化了下游逻辑。

**ConsumePayload 逻辑**（SafeIn >256KB 的 payload 预读）：
1. 将 buffer 所有权转移到 `payload_cache_[addr.offset]`
2. 不释放 buffer（后续组装 SearchResults 时使用）

### 2.7 OverlapScheduler — 单线程两阶段调度

**调度模型**：单线程顺序执行 Probe + Submit → Drain + Rerank。CPU/IO 重叠依靠内核：
Phase 1 中每个 cluster probe 完毕后 `Submit` flush，内核立即开始处理 SQ 中的请求；
Phase 2 开始 drain CQ 时，大部分 I/O 已完成或正在完成中。

**无应用层队列**：io_uring SQ/CQ 即为唯一队列，不需要 ReadQueue、SPSC queue。

```
┌──────────────────────────────────────────────────────────────────┐
│  单线程两阶段模型                                                  │
│                                                                  │
│  Phase 1: ProbeAndSubmit                                         │
│  ┌──────────────────────────────────────────────────────────────┐│
│  │ for each cluster (0..nprobe):                                ││
│  │   EnsureClusterLoaded(c)                                     ││
│  │   PrepareQuery(c)                                            ││
│  │   for batch(offset, 64):                                     ││
│  │     EstimateBatch → Classify                                 ││
│  │     non-SafeOut → pool.Acquire → pending_[buf] = {...}       ││
│  │                    reader.PrepRead(fd, buf, len, off)        ││
│  │   reader.Submit()   // flush: 内核开始执行本 cluster 的 I/O  ││
│  └──────────────────────────────────────────────────────────────┘│
│                                                                  │
│  Phase 2: DrainAndRerank                                         │
│  ┌──────────────────────────────────────────────────────────────┐│
│  │ while !pending_.empty():                                     ││
│  │   n = reader.WaitAndPoll(completions, 32)                    ││
│  │   for each completion:                                       ││
│  │     p = pending_.extract(buf)                                ││
│  │     type = InferType(p, dim)                                 ││
│  │     switch(type):                                            ││
│  │       VEC_ONLY → ConsumeVec → pool.Release(buf)              ││
│  │       ALL      → ConsumeAll → pool.Release(buf)              ││
│  │       PAYLOAD  → ConsumePayload (所有权转移, 不释放)          ││
│  └──────────────────────────────────────────────────────────────┘│
│                                                                  │
│  全部状态 local（单线程，无锁）:                                   │
│    pending_ map, BufferPool, RerankConsumer, ResultCollector     │
└──────────────────────────────────────────────────────────────────┘
```

```cpp
/// 用于 pending_ map：记录每个 in-flight buffer 的元信息
struct PendingRead {
    AddressEntry addr;      // 原始地址
    uint64_t read_offset;   // 本次读取的文件偏移
    uint32_t read_length;   // 本次读取的长度
};

class OverlapScheduler {
public:
    OverlapScheduler(index::IvfIndex& index,
                     AsyncReader& reader,
                     SearchConfig config);

    /// 执行完整搜索，返回 RAII 管理的结果
    SearchResults Search(const float* query_vec);

private:
    /// Phase 1: Probe 所有 cluster + Submit I/O
    void ProbeAndSubmit(SearchContext& ctx);

    /// Phase 2: Drain CQ + L2Sqr Rerank
    void DrainAndRerank(SearchContext& ctx);

    /// Phase 3: 补读 Uncertain payload
    void FetchMissingPayloads(SearchContext& ctx,
                              const std::vector<CollectorEntry>& results);

    /// Phase 4: 组装最终结果
    SearchResults AssembleResults(const std::vector<CollectorEntry>& results);

    /// 从 PendingRead 参数推断 ReadTaskType
    ReadTaskType InferType(const PendingRead& p, uint32_t dim) const;

    // ── 成员（全部单线程访问，无锁）──
    index::IvfIndex& index_;
    AsyncReader& reader_;
    SearchConfig config_;
    BufferPool buffer_pool_;
    RerankConsumer reranker_;
    std::unordered_map<uint8_t*, PendingRead> pending_;
};
```

**调度循环伪代码**：

```
Search(query):
  clusters = FindNearestClusters(query, nprobe)
  ctx = SearchContext(query, config)

  // ═══ Phase 1: Probe + Submit ═══
  ProbeAndSubmit(ctx)

  // ═══ Phase 2: Drain CQ + Rerank ═══
  DrainAndRerank(ctx)

  // ═══ Phase 3: 补读 Uncertain payload ═══
  results = ctx.collector().Finalize()    // sorted (dist, addr)[]
  FetchMissingPayloads(ctx, results)

  // ═══ Phase 4: 组装 ═══
  return AssembleResults(results)


ProbeAndSubmit(ctx):
  for c in clusters:
    EnsureClusterLoaded(c)
    pq = PrepareQuery(c)       // RaBitQ per-cluster prepare
    num = segment.GetNumRecords(c)

    // Probe: 以 64 条为 mini-batch
    for offset = 0; offset < num; offset += 64:
      batch = min(64, num - offset)
      code_ptrs = GetCodePtrs(c, offset, batch)

      float distances[64]
      EstimateDistanceBatch(pq, code_ptrs, batch, distances)

      for i in 0..batch:
        cls = conann.Classify(distances[i])

        if cls == SafeOut:
          ctx.stats().total_safe_out++
          continue

        addr = segment.GetAddress(c, offset + i)

        if cls == SafeIn:
          ctx.stats().total_safe_in++

          if addr.size <= safein_all_threshold:
            // SafeIn 小 record: 全量读取 (vec + payload)
            buf = pool.Acquire(addr.size)
            pending_[buf] = PendingRead{addr, addr.offset, addr.size}
            reader.PrepRead(fd, buf, addr.size, addr.offset)
          else:
            // SafeIn 大 record: 拆分
            vec_len = dim * sizeof(float)
            buf_v = pool.Acquire(vec_len)
            pending_[buf_v] = PendingRead{addr, addr.offset, vec_len}
            reader.PrepRead(fd, buf_v, vec_len, addr.offset)

            payload_len = addr.size - vec_len
            buf_p = pool.Acquire(payload_len)
            pending_[buf_p] = PendingRead{addr, addr.offset + vec_len, payload_len}
            reader.PrepRead(fd, buf_p, payload_len, addr.offset + vec_len)

        else:  // Uncertain
          ctx.stats().total_uncertain++
          vec_len = dim * sizeof(float)
          buf = pool.Acquire(vec_len)
          pending_[buf] = PendingRead{addr, addr.offset, vec_len}
          reader.PrepRead(fd, buf, vec_len, addr.offset)

      ctx.stats().total_probed += batch

    // 每个 cluster probe 完毕后 flush Submit
    submitted = reader.Submit()
    ctx.stats().total_io_submitted += submitted


DrainAndRerank(ctx):
  while !pending_.empty():
    IoCompletion completions[32]
    n = reader.WaitAndPoll(completions, 32)

    for i in 0..n:
      buf = completions[i].buffer
      p = pending_.extract(buf)
      type = InferType(p, dim)

      if type == VEC_ONLY:
        reranker.ConsumeVec(buf, p.addr)
        pool.Release(buf)
        ctx.stats().total_reranked++

      elif type == ALL:
        reranker.ConsumeAll(buf, p.addr)
        pool.Release(buf)       // 原始 ALL buffer 始终释放
        ctx.stats().total_reranked++

      elif type == PAYLOAD:
        reranker.ConsumePayload(buf, p.addr)
        ctx.stats().total_payload_prefetched++
        // buffer 所有权转移到 cache，不释放


FetchMissingPayloads(ctx, results):
  for r in results:
    if !reranker.HasPayload(r.addr.offset):
      // Uncertain 条目，需要补读 payload
      payload_len = r.addr.size - dim * sizeof(float)
      buf = pool.Acquire(payload_len)
      offset = r.addr.offset + dim * sizeof(float)
      pending_[buf] = PendingRead{r.addr, offset, payload_len}
      reader.PrepRead(fd, offset, payload_len, buf)
      ctx.stats().total_payload_fetched++

  reader.Submit()

  while !pending_.empty():
    completions = WaitAndPoll()
    for c in completions:
      buf = c.buffer
      p = pending_.extract(buf)
      reranker.CachePayload(p.addr.offset, unique_ptr(buf))


AssembleResults(results):
  SearchResults sr;
  for r in results:
    buf = reranker.TakePayload(r.addr.offset)
    // 所有 payload_cache 条目的 payload 均从 buf[0] 开始
    // (ALL buffer 在 ConsumeAll 中已 memcpy 拆分, PAYLOAD buffer 天然如此)
    payload = ParseDatum(buf)   // 解析单个 Datum
    sr.results_.push_back({r.distance, payload})
    sr.owned_buffers_.push_back(std::move(buf))

  reranker.CleanupUnusedCache(results)  // 惰性释放未进 TopK 的 SafeIn cache
  return sr
```

**InferType 逻辑**：

```cpp
ReadTaskType InferType(const PendingRead& p, uint32_t dim) const {
    uint32_t vec_bytes = dim * sizeof(float);
    if (p.read_length == vec_bytes)
        return ReadTaskType::VEC_ONLY;
    if (p.read_offset == p.addr.offset && p.read_length == p.addr.size)
        return ReadTaskType::ALL;
    return ReadTaskType::PAYLOAD;
}
```

## 3. 数据流详图

```
═══════════════ Phase 1: Probe + Submit ═══════════════

  for each cluster:
  ┌──────────────────────────────────────────────────────────────┐
  │  EnsureClusterLoaded → PrepareQuery                          │
  │  EstimateDistanceBatch → Classify                            │
  │                                                              │
  │  SafeOut ──→ skip                                            │
  │  SafeIn (≤256KB) → PrepRead ALL{fd, buf, addr.size, off}    │
  │  SafeIn (>256KB) → PrepRead VEC_ONLY + PrepRead PAYLOAD     │
  │  Uncertain ──────→ PrepRead VEC_ONLY{fd, buf, dim*4, off}   │
  │                                                              │
  │  pending_[buf] = PendingRead{addr, read_offset, read_length}│
  │  → reader.Submit()  // flush per cluster                     │
  └──────────────────────────────────────────────────────────────┘
         │
         │ io_uring SQ → kernel processing
         │
         ▼

═══════════════ Phase 2: Drain CQ + Rerank ═══════════════

  ┌──────────────────────────────────────────────────────────────┐
  │  while !pending_.empty():                                    │
  │    WaitAndPoll(completions, 32)                              │
  │    for each completion:                                      │
  │      extract PendingRead from pending_                       │
  │      InferType → dispatch:                                   │
  │                                                              │
  │      VEC_ONLY: L2Sqr → TryInsert → pool.Release(buf)        │
  │      ALL:      L2Sqr → TryInsert → memcpy payload           │
  │                → pool.Release(buf)                           │
  │      PAYLOAD:  → payload_cache (转移所有权)                   │
  └──────────────────────────────────────────────────────────────┘


═══════════════ Phase 3: 补读 Uncertain payload ═══════════════

  results = collector.Finalize()
  // results: [(dist, addr), ...] 按 distance 升序

  for r in results:
    if payload_cache.has(r.addr.offset):
      // SafeIn, payload 已在 cache 中 ✓
      continue
    else:
      // Uncertain, 需要补读 payload
      PrepRead PAYLOAD{fd, buf, payload_len, addr.offset + dim*4}

  Submit() → WaitAndPoll → payload_cache[addr.offset] = buffer


═══════════════ Phase 4: 组装 SearchResults (RAII) ═══════════════

  SearchResults sr;
  for r in results:
    buf = payload_cache.take(r.addr.offset)
    // 所有 cache 条目的 payload 均从 buf[0] 开始
    // (ALL 在 ConsumeAll 中已 memcpy 拆分, PAYLOAD 天然如此)
    payload = ParseDatum(buf)   // 解析单个 Datum 值
    sr.results_ += {r.distance, payload}
    sr.owned_buffers_ += buf     // buffer 所有权转移到 SearchResults

  // 惰性释放未命中 TopK 的 SafeIn payload cache
  cleanup_unused_cache()

  return sr    // 用户持有 sr, 析构时自动释放所有 buffer
```

## 4. Buffer 生命周期

```
┌──────────────────────────────────────────────────────────────┐
│  Buffer 生命周期（单线程，直接管理）                            │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  VEC_ONLY buffer:                                            │
│    Phase 1: pool.Acquire() → PrepRead → Submit               │
│    Phase 2: WaitAndPoll → L2Sqr → pool.Release()             │
│    生命周期: Acquire → I/O → Rerank → Release                │
│                                                              │
│  ALL buffer (SafeIn ≤256KB, 含 vec+payload):                 │
│    Phase 1: pool.Acquire(addr.size) → PrepRead → Submit      │
│    Phase 2: WaitAndPoll → L2Sqr → memcpy payload → Release() │
│    new_buf 生命周期: memcpy 新建 → cache → TopK→SearchResults │
│                       或: 被淘汰 → CleanupUnusedCache 惰性释放 │
│                                                              │
│  PAYLOAD buffer (SafeIn >256KB, 仅 payload):                 │
│    Phase 1: pool.Acquire() → PrepRead → Submit               │
│    Phase 2: WaitAndPoll → payload_cache (所有权转移)          │
│    生命周期: 转移到 cache → TopK → SearchResults              │
│              或: 不在 TopK → CleanupUnusedCache 释放          │
│                                                              │
│  PAYLOAD buffer (Uncertain, Phase 3 补读):                   │
│    Phase 3: pool.Acquire() → PrepRead → Submit → WaitAndPoll │
│    → payload_cache → SearchResults                           │
│                                                              │
│  SearchResults 析构:                                          │
│    ~SearchResults() → 释放所有 owned_buffers_                 │
│    用户持有期间, payload 指针有效                               │
│                                                              │
│  注: 单线程模型下，所有 pool.Acquire/Release 都是直接调用，     │
│  无需跨线程归还机制 (无 pool_return_q_)                       │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

## 5. 其他组件

### 5.1 L2Sqr — 精确距离计算

位于 `include/vdb/common/distance.h`，独立于 RaBitQ，用于 Rerank 精排。

```cpp
namespace vdb {

/// 计算两个向量的 L2 平方距离
/// @param a, b  原始浮点向量
/// @param dim   维度
/// @return sum((a[i] - b[i])^2)
float L2Sqr(const float* a, const float* b, uint32_t dim);

}  // namespace vdb
```

薄包装，委托 `simd::L2Sqr`（已存在 AVX2 + 标量回退）。

### 5.2 BufferPool

```cpp
class BufferPool {
public:
    /// 获取一个 buffer（pool 中有 capacity >= size 的则复用，否则 new）
    uint8_t* Acquire(uint32_t size);

    /// 归还一个 buffer
    void Release(uint8_t* buf);

private:
    // buffer + capacity 对
    std::vector<std::pair<std::unique_ptr<uint8_t[]>, uint32_t>> pool_;
};
```

### 5.3 ClusterStoreReader Codes 缓存

在 `EnsureClusterLoaded` 中新增 codes 全量缓存：

```cpp
struct ClusterData {
    // ... existing fields ...
    std::vector<uint8_t> codes_buffer;   // 全量 codes region
    bool codes_loaded = false;
};

const uint8_t* GetCodePtr(uint32_t cluster_id, uint32_t record_idx) const;
```

Probe 循环中的 `LoadCode` 变为纯内存访问，零系统调用。

### 5.4 DataFileReader fd 访问器

`OverlapScheduler` 需要 fd 传给 `PrepRead`，在 `DataFileReader` 中添加：

```cpp
/// 获取文件描述符（用于 io_uring 直接提交）
int fd() const { return fd_; }
```

## 6. CMake 集成

```cmake
# liburing — 必须依赖
find_library(URING_LIB uring REQUIRED)
if(NOT URING_LIB)
    message(FATAL_ERROR "liburing not found. Install with: sudo apt install liburing-dev")
endif()

# vdb_query library
add_library(vdb_query STATIC
    src/query/io_uring_reader.cpp
    src/query/pread_fallback_reader.cpp
    src/query/result_collector.cpp
    src/query/rerank_consumer.cpp
    src/query/overlap_scheduler.cpp
    src/query/buffer_pool.cpp
    src/common/distance.cpp
)
target_link_libraries(vdb_query PUBLIC
    vdb_index vdb_storage vdb_simd)
target_link_libraries(vdb_query PRIVATE ${URING_LIB})
```

**对比之前**：移除了 Taskflow FetchContent、`spsc_queue.cpp`、`read_queue.cpp`。

## 7. 已确定决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| - | 调度模型 | **单线程两阶段** | Phase 1 Probe+Submit → Phase 2 Drain+Rerank；内核处理 I/O 提供 CPU/IO 重叠；无线程、无锁、无队列 |
| - | I/O 后端 | **io_uring SQ/CQ** | SQ 替代应用层任务队列，CQ 替代完成队列；PrepRead+Submit 映射 io_uring 原语 |
| - | CQ 容量 | **4096 (IORING_SETUP_CQSIZE)** | Phase 1 全部 Probe 完才 drain CQ，需要容纳所有请求；默认 CQ=2×SQ 不够 |
| 8 | Payload 策略 | **混合策略 + SafeIn ALL** | SafeIn ≤256KB → ALL 一次读完；SafeIn >256KB → VEC_ONLY+PAYLOAD；Uncertain → Rerank 后批量补读 |
| - | ReadTaskType | **VEC_ONLY + ALL + PAYLOAD** | 3 种类型：Uncertain 用 VEC_ONLY，小 SafeIn 用 ALL，大 SafeIn 拆分 |
| - | SafeIn ALL 阈值 | **256KB** | ≤256KB 时 NVMe 延迟与 512B 同量级(~25μs)，省一半 IOPS 和 SQE 槽位；>256KB 拆分 |
| - | VecID | **移除** | SearchResult 不返回 ID，只返回 distance + payload |
| - | SearchResult 生命周期 | **RAII (SearchResults 类)** | 自动管理 payload buffer，离开作用域自动释放 |
| - | TopK 阈值更新 | **Phase 1 不更新** | Probe 使用初始 ConANN 阈值，不实时更新 TopK；~10-20% 额外 I/O (nprobe=8)，换取无 Rerank 穿插的简单性 |
| 9 | Probe 粒度 | mini-batch 64 条 | 与 address block 粒度一致 |
| - | 聚类原子性 | ≤ 1024 原子，> 1024 分批 | 小聚类省状态管理，大聚类避免 CPU 长时间霸占 |
| 1 | O_DIRECT | 初期 buffered | 简单，后续加 O_DIRECT 选项 |
| 5 | codes 加载 | 全量到内存 | Probe 零系统调用 |
| 13 | AsyncReader 抽象 | 薄封装 (PrepRead+Submit) | 映射 io_uring 原语，单 fd，调用方管理 buffer |
| - | liburing 依赖 | 构建必须，运行时降级 | 构建时 CMake REQUIRED；运行时 io_uring init 失败则警告 + fallback 到 pread |
| - | 精排距离计算 | `L2Sqr` 薄包装 in `common/` | 委托 `simd::L2Sqr`，不重复实现 |
| - | 错误处理 | 直接返回错误 + 调试信息 | 初期从简，不做 skip/retry |
| - | CQE user_data | **buffer 指针** | `user_data = (uint64_t)buffer`，CQE 返回后还原指针；配合 `pending_` map 查元信息 |
| - | 类型推断 | **从 PendingRead 参数推断** | `read_length==dim*4` → VEC_ONLY，`offset==addr.offset && length==addr.size` → ALL，其他 → PAYLOAD |
| - | ALL buffer 处理 | **拆分 (memcpy payload)** | ConsumeAll 中 memcpy payload 到新 buffer 入 cache，原始 ALL buffer 始终释放。代价一次 ≤256KB memcpy，换来 cache 统一 offset 0 |
| - | 被淘汰 cache 条目 | **惰性释放** | 不在 TryInsert 淘汰时立即释放，而是在搜索结束后 `CleanupUnusedCache` 统一释放 |
| - | Datum 结构 | **单个 Datum** | 初期 payload 为单个 Datum 值，非多列 Datum 序列。后续可扩展 |
| - | ConANN 测试策略 | **两阶段** | 初步测试硬编码 ConANN 阈值；后续集成 warmup 采样计算 |

## 8. 未来优化方向

| 优化 | 描述 | 预期收益 |
|------|------|----------|
| 多线程并发 (Taskflow) | 将 Probe/I/O/Rerank 拆分为 3 个并发 task，SPSC queue 解耦 | 大 nprobe 场景吞吐提升，真并行 |
| 交错式 Rerank | Phase 2 中穿插 Probe，实时更新 TopK 阈值减少 I/O | 减少 10-20% 无效 I/O |
| io_uring provided buffers | VEC_ONLY 使用内核管理的固定大小 buffer pool | 减少用户态 buffer 分配开销 |
| O_DIRECT | 绕过 page cache，避免大数据集污染 | 减少内存拷贝 |
| Early termination | TopK 满 + 剩余 cluster 距离 > 堆顶 + 2ε → 跳过 | 减少无效 probe |
| ALL 阈值自适应 | 根据运行时 I/O 延迟统计自动调整 safein_all_threshold | 适配不同存储设备 |
| Slab buffer pool | 按大小分级的 buffer 复用 | 减少 alloc/free |
| 并行 EnsureClusterLoaded | 并行加载 nprobe 个 cluster | 减少 Probe 前的串行加载延迟 |
