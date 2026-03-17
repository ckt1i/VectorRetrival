# Design: Query Pipeline (Phase 7)

> 权衡分析详见 [design-tradeoffs.md](./design-tradeoffs.md)

## 1. 架构总览

```
SearchRequest(query_vec, top_k, nprobe)
  │
  ▼
┌────────────────────────────────────────────────────────────────────┐
│  OverlapScheduler — 交错式主循环                                     │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  主循环: while (有cluster未probe || 队列非空 || InFlight>0)    │   │
│  │                                                             │   │
│  │    ┌──────────┐    ┌──────────┐    ┌──────────┐             │   │
│  │    │ 1. Probe │───▶│ 2. I/O   │───▶│ 3.Rerank │──┐          │   │
│  │    │ NextBatch│    │ Submit   │    │ Process  │  │          │   │
│  │    └──────────┘    └──────────┘    └──────────┘  │          │   │
│  │        ▲                                         │          │   │
│  │        └─────────────────────────────────────────┘          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼ 主循环结束                            │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 2: 补读 Uncertain payload                             │   │
│  │  TopK 中 payload=nullptr 的条目 → 批量 PAYLOAD 读取            │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 3: 组装 SearchResults (RAII)                          │   │
│  │  从 payload_cache 查找 → 填入指针 → 返回                       │   │
│  └─────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────┘
```

**执行模型**：交错式流水线，Probe / I/O / Rerank 在主循环中每轮迭代交替执行，深度重叠。

**主循环（交错执行）**：
- Step 1: Probe 一个批次（64 条或一个小聚类），产出 ReadTask → Push Qv/Qp
- Step 2: 从 Qv/Qp 取 task → Submit io_uring
- Step 3: Poll 已完成的 I/O → VEC_ONLY/ALL 做 L2Sqr 精排 → 更新 TopK；ALL 中 payload 直接入 cache；PAYLOAD 完成也入 cache
- 重复直到所有 cluster 已 probe 且队列清空且无 in-flight I/O

**Phase 2: 补读 Uncertain payload**：
- Finalize 排序 TopK 结果
- 检查哪些条目的 payload 不在 cache 中（Uncertain 条目）
- 批量提交 PAYLOAD 读取 → 等待完成

**Phase 3: 组装结果**：
- 从 payload_cache 查找每条 TopK 条目的 payload
- 组装为 SearchResults（RAII，自动管理 buffer 生命周期）

**I/O 时序图**：

```
假设 nprobe=4, 每cluster ~200条, dim=128

时间 ─────────────────────────────────────────────────────────────────►

CPU:   [Probe C0] [Probe C1]  [Probe C2]  [Probe C3]
        ↓↓↓        ↓↓↓         ↓↓↓         ↓↓↓
Qv:    ALL×30     ALL×25      ALL×35      ALL×28    (SafeIn ≤256KB)
       VEC×50     VEC×35      VEC×55      VEC×42    (Uncertain)
Qp:    PAY×2      PAY×1       PAY×3       PAY×1     (SafeIn >256KB payload)

I/O:   [──Qv C0──][──Qv C1──][──Qv C1+C2──][──Qv C3──][──Qp──][Qp]
                ↓           ↓              ↓          ↓
Rerank:   [L2Sqr C0]  [L2Sqr C1]    [L2Sqr C2]  [L2Sqr C3]

                                                          Phase 2:
                                                    [补读 Uncertain payload]
                                                          ↓
返回:                                                 [组装 SearchResults]

关键: CPU Probe 和 I/O 读取深度重叠
      Rerank 和下一批 Qv/Qp 读取深度重叠
      SafeIn ≤256KB 通过 ALL 一次读完 vec+payload, 省一半 IOPS
      SafeIn >256KB 的 payload 在 Rerank 期间通过 Qp 后台预读
```

## 2. 组件详细设计

### 2.1 AsyncReader — I/O 后端接口

薄封装设计（权衡 13：方案 A），调用方管理 fd 和 buffer。

```cpp
namespace vdb::query {

/// 单次 I/O 请求
struct IoRequest { 
    int fd;                // 文件描述符
    uint64_t offset;       // 读取偏移
    uint32_t length;       // 读取长度
    uint8_t* buffer;       // 输出 buffer（调用方预分配）
    // user_data = (uint64_t)buffer，CQE 返回后直接还原为 buffer 指针
};

/// I/O 完成结果
struct IoCompletion {
    uint8_t* buffer;       // 从 CQE user_data 还原的 buffer 指针
    int32_t result;        // 成功: 读取字节数, 失败: 负 errno
};

class AsyncReader {
public:
    virtual ~AsyncReader() = default;

    /// 提交一批读取请求
    /// @return 实际提交数（SQ 满时 < count）
    virtual uint32_t Submit(const IoRequest* requests, uint32_t count) = 0;

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
- `Init(queue_depth=64)` → `io_uring_setup`
- `Submit` → `io_uring_prep_read` + `io_uring_submit`
- `Poll` → `io_uring_peek_cqe` 循环
- `WaitAndPoll` → `io_uring_wait_cqe` + 消费所有就绪的 CQE

**PreadFallbackReader**（运行时降级）：
- 当 io_uring 初始化失败时（如内核不支持），打印警告并自动降级到此实现
- `Submit` → 同步执行 `pread` → 结果存入内部完成队列
- `Poll` → 从内部队列返回
- `InFlight` → 始终返回 0（同步完成）

### 2.2 ReadQueue — 双优先级队列

```cpp
class ReadQueue {
public:
    explicit ReadQueue(uint32_t max_capacity = 256);

    /// 入队（根据 task.priority 自动分发到 Qv 或 Qp）
    /// @return false if Qv is full (back-pressure signal to pause Probe)
    bool Push(ReadTask task);

    /// SafeIn payload 入队（总是进 Qp）
    void PushQp(ReadTask task);

    /// 批量出队（Qv 优先，余量取 Qp）
    uint32_t PopBatch(ReadTask* out, uint32_t max_count);

    uint32_t Size() const;
    uint32_t SizeQv() const;
    uint32_t SizeQp() const;
    bool Empty() const;
    bool QvFull() const;

private:
    uint32_t max_capacity_;     // Qv 容量上限 = 256
    std::deque<ReadTask> qv_;   // 高优先: VEC_ONLY（向量读取，用于 Rerank）
    std::deque<ReadTask> qp_;   // 低优先: PAYLOAD（SafeIn 的 payload 预读）
};
```

**Qv/Qp 双队列职责**：
- **Qv（高优先）**：存放 ALL 和 VEC_ONLY 任务——SafeIn 小 record 全量读取 (ALL) 或 Uncertain 仅向量读取 (VEC_ONLY)，用于 L2Sqr 精排
- **Qp（低优先）**：存放 PAYLOAD 任务——SafeIn 大 record (>256KB) 的 payload 预读，在 Qv 有余量时才读取

**PopBatch 策略**：Qv 全取优先（权衡 3：方案 A），余量从 Qp 取。

**Back-pressure**：Qv 容量上限 256。当 Qv 满时 `Push` 返回 false，Scheduler 暂停 Probe，让 I/O 先消耗。Qp 无容量限制。

### 2.3 ReadTaskType

```cpp
enum class ReadTaskType : uint8_t {
    VEC_ONLY,   // 只读向量部分 (dim * sizeof(float) 字节)
    ALL,        // 全量读取 (vec + payload, 用于小 record 的 SafeIn)
    PAYLOAD,    // 只读 payload 部分 (从 offset + dim*4 开始)
};
```

**SafeIn 全量读取阈值**：256KB (`safein_all_threshold`)。
- record_size ≤ 256KB 时，NVMe 读取延迟与 VEC_ONLY (512B) 在同一量级（~25μs），且省一半 IOPS 和 SQE 槽位
- record_size > 256KB 时，ALL 占用 SQE 槽位过久（>50μs），拆分为 VEC_ONLY + PAYLOAD 避免阻塞 Qv

**分类与 ReadTask 生成规则**：

```
Probe 产出:
  SafeOut    → skip

  SafeIn + addr.size ≤ 256KB:
             → ALL{addr, offset=addr.offset, length=addr.size}   → Push Qv
               (一次 I/O 读完 vec + payload)

  SafeIn + addr.size > 256KB:
             → VEC_ONLY{addr, dim*4}       → Push Qv
               PAYLOAD{addr, offset=dim*4}  → Push Qp
               (拆分：向量快速到达 Rerank, payload 后台预读)

  Uncertain  → VEC_ONLY{addr, dim*4}       → Push Qv

Phase 2 补读:
  TopK 中 payload 不在 cache 的条目 (Uncertain 或被淘汰后重新需要的)
             → PAYLOAD{addr, offset=dim*4}  → Submit 直接提交
```

### 2.4 ResultCollector — Top-K 收集

```cpp
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
    struct CollectorEntry {
        float distance;
        AddressEntry addr;
        bool operator<(const CollectorEntry& o) const {
            return distance < o.distance;  // max-heap
        }
    };
    // max-heap: 堆顶是最大距离
    std::priority_queue<CollectorEntry> heap_;
};
```

**无 VecID**：堆只存 `(distance, AddressEntry)`。不返回 ID，最终结果只包含距离和原始数据。

### 2.5 SearchResult 与 SearchResults (RAII)

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

### 2.6 SearchContext — 查询上下文

```cpp
struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 8;
    uint32_t probe_batch_size = 64;              // mini-batch 大小（大聚类分批用）
    uint32_t cluster_atomic_threshold = 1024;    // ≤ 此值的聚类原子 probe
    uint32_t io_queue_depth = 64;                // io_uring SQ 深度
    uint32_t qv_capacity = 256;                  // Qv back-pressure 阈值
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
    uint32_t total_payload_补读 = 0;         // Phase 2 Uncertain payload 补读数
    double   probe_time_ms = 0;
    double   io_time_ms = 0;
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

### 2.7 RerankConsumer — 流式 Rerank + Payload Cache

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
1. 从 `read.buffer` 解析 raw vector（前 `dim * 4` 字节）
2. `L2Sqr(query_vec, parsed_vec, dim)` 计算精确距离
3. `collector.TryInsert(exact_dist, addr)`
4. 调用方释放 buffer

**ConsumeAll 逻辑**（SafeIn ≤256KB 的全量读取）：
1. 从 `read.buffer` 解析 raw vector（`buffer[0..dim*4]`）
2. `L2Sqr(query_vec, parsed_vec, dim)` 计算精确距离
3. `collector.TryInsert(exact_dist, addr)`
4. 若进入 TopK：
   - `memcpy` payload 部分（`buffer[dim*4..]`）到新分配的 payload buffer
   - `payload_cache_[addr.offset] = new_payload_buf`（payload 从 offset 0 开始）
   - 返回 false（**原始 ALL buffer 始终由调用方释放**）
5. 若未进入 TopK：不做额外操作
6. **调用方始终释放原始 ALL buffer**（无论是否进入 TopK）

> 设计决策：ALL buffer 拆分而非保留整个 buffer。这确保 payload_cache 中所有条目
> 的 payload 均从 offset 0 开始，AssembleResults 无需区分 buffer 类型。
> 代价是一次 memcpy（≤256KB），但简化了下游逻辑。

**ConsumePayload 逻辑**（SafeIn >256KB 的 payload 预读）：
1. 将 buffer 所有权转移到 `payload_cache_[addr.offset]`
2. 不释放 buffer（后续组装 SearchResults 时使用）

### 2.8 OverlapScheduler — 交错式调度主循环

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
    /// Probe 一个批次（64条 或 一个小 cluster）
    void ProbeNextBatch(SearchContext& ctx);

    /// 主循环：交错 Probe + I/O + Rerank
    void InterleavedLoop(SearchContext& ctx);

    /// Phase 2: 补读 Uncertain payload
    void FetchMissingPayloads(SearchContext& ctx,
                              const std::vector<CollectorEntry>& results);

    /// Phase 3: 组装最终结果
    SearchResults AssembleResults(const std::vector<CollectorEntry>& results);

    /// 从 PendingRead 参数推断 ReadTaskType（无需显式存储 type）
    /// - read_length == dim * sizeof(float)              → VEC_ONLY
    /// - read_offset == addr.offset && read_length == addr.size → ALL
    /// - 其他（read_offset == addr.offset + dim*4）       → PAYLOAD
    ReadTaskType InferType(const PendingRead& p, uint32_t dim) const;

    ReadQueue queue_;
    BufferPool buffer_pool_;
    RerankConsumer reranker_;

    /// buffer 指针 → 元信息映射
    /// Submit 时插入，Poll 完成时查找并移除
    std::unordered_map<uint8_t*, PendingRead> pending_;
};
```

**调度循环伪代码（交错模式）**：

```
Search(query):
  clusters = FindNearestClusters(query, nprobe)
  for c in clusters: EnsureClusterLoaded(c)
  ctx = SearchContext(query, config)

  // ═══ 交错主循环 ═══
  InterleavedLoop(ctx)

  // ═══ Phase 2: 补读 Uncertain payload ═══
  results = ctx.collector().Finalize()    // sorted (dist, addr)[]
  FetchMissingPayloads(ctx, results)

  // ═══ Phase 3: 组装 ═══
  return AssembleResults(results)


InterleavedLoop(ctx):
  cluster_idx = 0
  record_offset = 0

  while true:
    // ── Step 1: Probe 一个批次（如果 Qv 未满且有 cluster 剩余）──
    if cluster_idx < nprobe && !queue.QvFull():
      c = clusters[cluster_idx]
      num = segment.GetNumRecords(c)

      if num <= cluster_atomic_threshold:
        ProbeClusterFull(ctx, c)
        cluster_idx++
      else:
        batch = min(64, num - record_offset)
        ProbeClusterBatch(ctx, c, record_offset, batch)
        record_offset += batch
        if record_offset >= num:
          cluster_idx++; record_offset = 0

    // ── Step 2: Submit I/O（Qv 优先）──
    can_submit = io_queue_depth - reader.InFlight()
    if can_submit > 0 && !queue.Empty():
      tasks = queue.PopBatch(can_submit)    // Qv 优先, 余量取 Qp
      for t in tasks:
        buf = pool.Acquire(t.read_length)
        pending_[buf] = PendingRead{t.addr, t.read_offset, t.read_length}
        reader.Submit(IoRequest{fd, t.read_offset, t.read_length, buf})

    // ── Step 3: Poll 完成项 ──
    n = reader.Poll(completions, 32)
    if n == 0 && reader.InFlight() > 0 && (cluster_idx >= nprobe || queue.QvFull()):
      // 没有更多 Probe 工作（或 Qv 满），阻塞等 I/O
      n = reader.WaitAndPoll(completions, 32)

    // ── Step 4: 处理完成项（从 pending_ 推断类型）──
    for i in 0..n:
      buf = completions[i].buffer          // 从 CQE user_data 还原
      p = pending_.extract(buf)            // 查找并移除元信息
      type = InferType(p, dim)

      if type == VEC_ONLY:
        reranker.ConsumeVec(buf, p.addr)
        pool.Release(buf)

      elif type == ALL:
        reranker.ConsumeAll(buf, p.addr)   // 内部 memcpy payload 到新 buffer
        pool.Release(buf)                  // ALL buffer 始终释放

      elif type == PAYLOAD:
        reranker.ConsumePayload(buf, p.addr)
        // buffer 所有权转移到 cache，不释放

    // ── 终止条件 ──
    if cluster_idx >= nprobe && queue.Empty() && reader.InFlight() == 0:
      break


FetchMissingPayloads(ctx, results):
  for r in results:
    if !reranker.HasPayload(r.addr.offset):
      // Uncertain 条目，需要补读 payload
      payload_len = r.addr.size - dim * sizeof(float)
      buf = pool.Acquire(payload_len)
      offset = r.addr.offset + dim * sizeof(float)
      pending_[buf] = PendingRead{r.addr, offset, payload_len}
      reader.Submit(IoRequest{fd, offset, payload_len, buf})
      ctx.stats().total_payload_补读++

  while reader.InFlight() > 0:
    completions = WaitAndPoll()
    for c in completions:
      buf = c.buffer
      p = pending_.extract(buf)
      reranker.CachePayload(p.addr.offset, buf)


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

**Probe 内部流程（mini-batch 64 条）**：

```
ProbeClusterBatch(ctx, cluster_id, offset, count):
  // codes 已在 EnsureClusterLoaded 时缓存到内存
  code_ptrs = GetCodePtrs(cluster_id, offset, count)

  // mini-batch 距离估算
  float distances[64]
  EstimateDistanceBatch(pq, code_ptrs, count, distances)

  // 分类 + 生成 ReadTask
  for i in 0..count:
    cls = conann.Classify(distances[i])
    if cls == SafeOut:
      ctx.stats().total_safe_out++
      continue

    addr = segment.GetAddress(cluster_id, offset + i)

    if cls == SafeIn:
      ctx.stats().total_safe_in++

      if addr.size <= safein_all_threshold:
        // SafeIn 小 record: 全量读取 → Qv (一次 I/O)
        queue.Push(ReadTask{
          .addr = addr,
          .task_type = ALL,
          .read_offset = addr.offset,
          .read_length = addr.size,
          .priority = 0  // Qv
        })
      else:
        // SafeIn 大 record: 拆分为向量 + payload
        queue.Push(ReadTask{
          .addr = addr,
          .task_type = VEC_ONLY,
          .read_offset = addr.offset,
          .read_length = dim * sizeof(float),
          .priority = 0  // Qv
        })
        queue.PushQp(ReadTask{
          .addr = addr,
          .task_type = PAYLOAD,
          .read_offset = addr.offset + dim * sizeof(float),
          .read_length = addr.size - dim * sizeof(float),
          .priority = 1  // Qp
        })

    else:  // Uncertain
      ctx.stats().total_uncertain++
      queue.Push(ReadTask{
        .addr = addr,
        .task_type = VEC_ONLY,
        .read_offset = addr.offset,
        .read_length = dim * sizeof(float),
        .priority = 0  // Qv
      })

  ctx.stats().total_probed += count
```

## 3. 数据流详图

```
═══════════════ 交错主循环 ═══════════════

每轮迭代:

  Step 1: Probe 一个批次 (64条 或 一个小cluster)
  ┌──────────────────────────────────────────────────────────────┐
  │  EstimateDistanceBatch → Classify                            │
  │                                                              │
  │  SafeOut ──→ skip                                            │
  │                                                              │
  │  SafeIn (≤256KB) → ALL{addr, addr.size}       ──→ Push Qv   │
  │                     (一次读完 vec + payload)                   │
  │                                                              │
  │  SafeIn (>256KB) → VEC_ONLY{addr, dim*4}      ──→ Push Qv   │
  │                    PAYLOAD{addr, offset=dim*4} ──→ Push Qp   │
  │                                                              │
  │  Uncertain ──────→ VEC_ONLY{addr, dim*4}       ──→ Push Qv   │
  └──────────────────────────────────────────────────────────────┘
                    │                    │
                    ▼                    ▼
              ┌──────────┐        ┌──────────┐
              │    Qv    │        │    Qp    │
              │ (高优先)  │        │ (低优先)  │
              │ ALL +    │        │ PAYLOAD  │
              │ VEC_ONLY │        │ (大SafeIn)│
              │ 上限 256  │        │ 无上限   │
              └────┬─────┘        └────┬─────┘
                   │                   │
                   └───────┬───────────┘
                           ▼
  Step 2: I/O Submit (Qv 优先, 余量取 Qp)
  ┌──────────────────────────────────────────────────────────┐
  │  can_submit = io_queue_depth - InFlight()                │
  │  PopBatch(Qv first, then Qp, up to can_submit)          │
  │  → AsyncReader::Submit()                                  │
  └──────────────────────────────────────────────────────────┘
                           │
                           ▼
  Step 3: Poll 完成项
  ┌──────────────────────────────────────────────────────────┐
  │  Poll() / WaitAndPoll()                                  │
  │  → IoCompletion[]                                        │
  └──────────────────────────────────────────────────────────┘
                           │
                           ▼
  Step 4: 处理完成项（从 pending_ map 推断类型）
  ┌──────────────────────────────────────────────────────────┐
  │  buf = CQE.user_data → 还原 buffer 指针                   │
  │  p = pending_.extract(buf) → 取出 PendingRead 元信息      │
  │  type = InferType(p, dim)                                │
  │                                                          │
  │  VEC_ONLY (Uncertain / SafeIn>256KB 的向量):              │
  │    parse vec → L2Sqr(query, vec, dim) → exact_dist       │
  │    collector.TryInsert(exact_dist, addr)                  │
  │    release buffer → pool                                 │
  │                                                          │
  │  ALL (SafeIn ≤256KB):                                    │
  │    parse vec (buffer[0..dim*4]) → L2Sqr → exact_dist     │
  │    collector.TryInsert(exact_dist, addr)                  │
  │    if 进入 TopK:                                          │
  │      memcpy buffer[dim*4..] → new_buf (仅 payload)       │
  │      payload_cache[addr.offset] = new_buf                │
  │    release original buffer → pool (始终释放原始 buffer)    │
  │                                                          │
  │  PAYLOAD (SafeIn >256KB 的 payload):                     │
  │    payload_cache[addr.offset] = buffer                   │
  │    (不释放 buffer, 转移所有权到 cache)                     │
  │                                                          │
  └──────────────────────────────────────────────────────────┘


═══════════════ Phase 2: 补读 Uncertain payload ═══════════════

  results = collector.Finalize()
  // results: [(dist, addr), ...] 按 distance 升序

  for r in results:
    if payload_cache.has(r.addr.offset):
      // SafeIn, payload 已在 cache 中 ✓
      continue
    else:
      // Uncertain, 需要补读 payload
      Submit PAYLOAD{addr, offset=dim*4, length=payload_size}

  while InFlight() > 0:
    WaitAndPoll → payload_cache[addr.offset] = buffer


═══════════════ Phase 3: 组装 SearchResults (RAII) ═══════════════

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
│  Buffer 生命周期                                              │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  VEC_ONLY buffer:                                            │
│    pool.Acquire() → Submit → Complete → L2Sqr → Release()    │
│    生命周期: I/O 完成 + Rerank 后立即回收                      │
│                                                              │
│  ALL buffer (SafeIn ≤256KB, 含 vec+payload):                 │
│    pool.Acquire(addr.size) → Submit → Complete → L2Sqr       │
│    进入 TopK → memcpy payload 到 new_buf → cache new_buf      │
│    原始 ALL buffer 始终 Release() (无论是否进入 TopK)          │
│    new_buf 生命周期: cache → 若在最终 TopK → 转移到 SearchResults │
│                       若被后续淘汰 → CleanupUnusedCache 惰性释放 │
│                                                              │
│  PAYLOAD buffer (SafeIn >256KB, 仅 payload):                 │
│    pool.Acquire() → Submit → Complete → payload_cache        │
│    生命周期: 转移到 cache → 若在 TopK 中, 转移到 SearchResults │
│              若不在 TopK, CleanupUnusedCache 释放              │
│                                                              │
│  PAYLOAD buffer (Uncertain, Phase 2 补读):                   │
│    pool.Acquire() → Submit → Complete → payload_cache        │
│    生命周期: 转移到 cache → 转移到 SearchResults               │
│                                                              │
│  SearchResults 析构:                                          │
│    ~SearchResults() → 释放所有 owned_buffers_                 │
│    用户持有期间, payload 指针有效                               │
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

初期标量实现，后续可加 AVX2/AVX-512 优化路径。

### 5.2 BufferPool

```cpp
class BufferPool {
public:
    explicit BufferPool(uint32_t max_buffers, uint32_t default_size = 4096);

    /// 获取一个 buffer（如果 pool 空则 new）
    uint8_t* Acquire(uint32_t size);

    /// 归还一个 buffer
    void Release(uint8_t* buf);

private:
    std::vector<std::unique_ptr<uint8_t[]>> pool_;
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
    src/query/read_queue.cpp
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

## 7. 已确定决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| - | 调度模型 | **交错式流水线** | Probe/I/O/Rerank 每轮交替执行，深度重叠，最大化 CPU 和 I/O 利用率 |
| 8 | Payload 策略 | **混合策略 + SafeIn ALL** | SafeIn ≤256KB → ALL 一次读完(Qv)；SafeIn >256KB → VEC_ONLY(Qv)+PAYLOAD(Qp)；Uncertain → Rerank 后批量补读 |
| - | ReadTaskType | **VEC_ONLY + ALL + PAYLOAD** | 3 种类型：Uncertain 用 VEC_ONLY，小 SafeIn 用 ALL，大 SafeIn 拆分 |
| - | SafeIn ALL 阈值 | **256KB** | ≤256KB 时 NVMe 延迟与 512B 同量级(~25μs)，省一半 IOPS 和 SQE 槽位；>256KB 拆分避免阻塞 Qv |
| - | VecID | **移除** | SearchResult 不返回 ID，只返回 distance + payload |
| - | SearchResult 生命周期 | **RAII (SearchResults 类)** | 自动管理 payload buffer，离开作用域自动释放 |
| - | Qv back-pressure | **容量 256** | Qv 满时暂停 Probe，让 I/O 先消耗 |
| 9 | Probe 粒度 | mini-batch 64 条 | 与 address block 粒度一致 |
| - | 聚类原子性 | ≤ 1024 原子，> 1024 分批 | 小聚类省状态管理，大聚类避免 CPU 长时间霸占 |
| 1 | O_DIRECT | 初期 buffered | 简单，后续加 O_DIRECT 选项 |
| 5 | codes 加载 | 全量到内存 | Probe 零系统调用 |
| 13 | AsyncReader 抽象 | 薄封装 | 单 fd，调用方管理 buffer |
| - | liburing 依赖 | 构建必须，运行时降级 | 构建时 CMake REQUIRED；运行时 io_uring init 失败则警告 + fallback 到 pread |
| - | 精排距离计算 | `L2Sqr` 独立函数 in `common/` | 不依赖 RaBitQ，语义清晰，可独立 SIMD 优化 |
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
| 模式 Y：攒批 Rerank | 积累完成项后批量 L2Sqr（prefetch 优化） | L2Sqr 吞吐提升 |
| O_DIRECT | 绕过 page cache，避免大数据集污染 | 减少内存拷贝 |
| Early termination | TopK 满 + 剩余 cluster 距离 > 堆顶 + 2ε → 跳过 | 减少无效 probe |
| ALL 阈值自适应 | 根据运行时 I/O 延迟统计自动调整 safein_all_threshold | 适配不同存储设备 |
| Slab buffer pool | 按大小分级的 buffer 复用 | 减少 alloc/free |
