Phase 7 查询流水线 — 细化设计探索
整体数据流
先明确端到端的数据流路径和现有接口的衔接点：


                          ┌─────────────────────────────────────────────┐
                          │             SearchContext                    │
                          │  query_vec, top_k, nprobe, result_collector │
                          └────────────────┬────────────────────────────┘
                                           │
                    ┌──────────────────────┤
                    ▼                      ▼
          ┌─────────────────┐   ┌──────────────────────┐
          │  IvfIndex::      │   │  Warmup (一次性)     │
          │  FindNearest-   │   │  EnsureClusterLoaded │
          │  Clusters()     │   │  for top clusters    │
          └────────┬────────┘   └──────────────────────┘
                   │ ClusterID[nprobe]
                   ▼
          ┌─────────────────────────────────────────────┐
          │  ProbeCluster (per cluster)                  │
          │    PrepareQuery() → EstimateDistanceBatch()  │
          │    → ConANN::Classify() → Candidate[]       │
          │    → ReadTask 生成 → Push to Qv/Qp          │
          └─────────────────┬───────────────────────────┘
                            │ ReadTask
                            ▼
          ┌──────────────────────────────────┐
          │  ReadQueue (Qv优先 → Qp)          │
          │  Pop() → batch of ReadTasks      │
          └─────────────┬────────────────────┘
                        │
                        ▼
          ┌──────────────────────────────────┐
          │  IoUringReader / PreadFallback   │
          │  Submit() → PollCompletions()   │
          └─────────────┬────────────────────┘
                        │ CompletedRead[]
                        ▼
          ┌──────────────────────────────────┐
          │  RerankConsumer                   │
          │  VEC_ONLY → L2Sqr → 确认TopK?   │
          │    → yes: BACK task → Qp         │
          │    → no: discard                  │
          │  ALL/FRONT → 解析vec+payload     │
          │    → ResultCollector.TryInsert() │
          └──────────────────────────────────┘
1. IoUringReader / PreadFallback — I/O 后端
核心设计问题：接口抽象层次

你现有的 DataFileReader 已经用 pread + fd 读取，而 io_uring 需要直接操作 fd。有两种设计路径：


方案 A: 薄封装                          方案 B: 厚封装
┌───────────────────────┐       ┌───────────────────────┐
│  AsyncReader (接口)    │       │  AsyncReader (接口)    │
│                       │       │                       │
│  Submit(fd, offset,   │       │  Submit(ReadTask[])   │
│         len, buf)     │       │  Poll() → Completed[] │
│  Poll() → results     │       │                       │
│                       │       │  (内部: 知道 dat fd,   │
│  (caller管理buffer,   │       │   管理buffer pool,     │
│   知道fd和offset)     │       │   自动拼接 ReadTask    │
└───────────────────────┘       │   → iovec)            │
                                └───────────────────────┘
方案 A (薄封装)	方案 B (厚封装)
复杂度	Reader 逻辑简单，Scheduler 复杂	Reader 自包含，Scheduler 简单
buffer 管理	调用方分配/回收	内部 pool
测试	更容易 mock	需要真实 fd 或 fake fd
扩展到多文件	调用方传 fd	需内部维护 fd 映射
我的建议：方案 A（薄封装），因为你目前只有一个 .dat 文件（单 fd），buffer 管理可以放在 Scheduler 里集中控制。

权衡 1：Direct I/O (O_DIRECT)

io_uring 配合 O_DIRECT 可以绕过 page cache，但有对齐要求（512B 或 4KB 对齐的 offset 和 buffer）。你的页对齐设计（page_size=4096）天然满足 offset 对齐，但 buffer 也需 4KB 对齐。


不用 O_DIRECT:
  + 简单，buffer 无对齐要求
  + 热数据自动缓存在 page cache
  - io_uring 提交 → 内核复制到 page cache → 再复制到 user buffer (两次复制)

用 O_DIRECT:
  + 零拷贝到用户态 buffer
  + 对大数据集避免污染 page cache
  - buffer 必须 4KB 对齐 (AlignedAlloc 已经有 64B 对齐，需改为 4KB)
  - 小读取 (<4KB) 必须 round up，浪费带宽
你需要决定：初期是否先不用 O_DIRECT，走 buffered I/O？（推荐初期 buffered，后续加 O_DIRECT 选项）

权衡 2：io_uring queue depth


queue_depth = 32:  适合 nprobe 小 (≤8) 的场景，内存开销低
queue_depth = 128: 适合大规模探测，SSD 能打满 IOPS
queue_depth = 256: 接近 NVMe 设备极限，需要更大 buffer pool
建议作为 Init() 参数，默认 64。

接口草案：


class AsyncReader {
public:
    virtual ~AsyncReader() = default;

    /// 提交一批读取请求
    /// @param fd       文件描述符
    /// @param tasks    ReadTask 数组（含 read_offset, read_length）
    /// @param buffers  预分配的输出 buffer（一一对应）
    /// @param count    任务数
    /// @return 实际提交数（可能因 SQ 满而 < count）
    virtual uint32_t Submit(int fd,
                            const ReadTask* tasks,
                            uint8_t** buffers,
                            uint32_t count) = 0;

    /// 轮询已完成的请求
    /// @param out       输出完成结果
    /// @param max_count 最多返回数
    /// @return 实际完成数
    virtual uint32_t Poll(CompletedRead* out, uint32_t max_count) = 0;

    /// 当前在途（submitted但未complete）的请求数
    virtual uint32_t InFlight() const = 0;
};

class IoUringReader : public AsyncReader { ... };
class PreadFallbackReader : public AsyncReader { ... };

2. ReadQueue — 双优先级队列
    这是一个相对直接的组件，但有几个细节：


    ┌──────────────────────────────────────┐
    │  ReadQueue                           │
    │                                      │
    │  ┌─────────┐     ┌─────────┐       │
    │  │   Qv    │     │   Qp    │       │
    │  │ (FIFO)  │     │ (FIFO)  │       │
    │  │ VEC_ONLY│     │ PAYLOAD │       │
    │  │ ALL     │     │ BACK    │       │
    │  │ FRONT   │     │         │       │
    │  └────┬────┘     └────┬────┘       │
    │       │               │             │
    │       └───────┬───────┘             │
    │               ▼                     │
    │         Pop(): Qv first             │
    │         PopBatch(n): 优先 Qv        │
    │                                      │
    │  Size(), Empty()                     │
    │  SizeQv(), SizeQp()                 │
    └──────────────────────────────────────┘
    权衡 3：PopBatch 的混合策略

    当 Scheduler 需要一批 tasks 提交给 io_uring 时，如何混合 Qv 和 Qp？


    策略 A: Qv 全取完再取 Qp
      pop(batch_size=32):
        先取 min(Qv.size(), 32) 个 Qv
        如果还有余量，取 Qp 补满

    策略 B: 按比例混合（如 3:1）
      每 4 个 pop 取 3 个 Qv + 1 个 Qp
      确保 payload 不被完全饿死

    策略 C: Qv-only 直到阈值
      Qv.size() > high_watermark → 只取 Qv
      否则 Qv/Qp 交替
    推荐策略 A（简单、Qv 优先语义清晰）。如果 Qp 饥饿是实际问题，后续再加比例控制。

    权衡 4：队列是否有容量上限？

    无限队列 vs 有界队列（back-pressure）：


    无界队列: Probe 跑完所有 candidates 才开始 I/O
      + 简单
      - 内存: nprobe × avg_cluster_size × sizeof(ReadTask) 可能很大

    有界队列: Probe 被 Qv 满时阻塞（协作式 yield）
      + 内存可控
      - Scheduler 需要更复杂的状态管理
    考虑到你是单线程协作式调度，有界队列的 back-pressure 自然就是"Probe 暂停，去做 I/O + Rerank"。建议设一个 max_queue_size（默认 256），Probe 看到队列满时 yield。

3. Probe 阶段 — 与现有接口的衔接
    这是流水线中计算最密集的部分。看你现有的接口链：
    Per probe cluster:
      1. segment.EnsureClusterLoaded(cluster_id)        // lazy load codes+addresses
      2. estimator.PrepareQuery(query, centroid, rotation) // per query×cluster
      3. For i in 0..num_records:
          code = segment.LoadCode(cluster_id, i)        // pread from .clu
          dist = estimator.EstimateDistance(pq, code)    // XOR + popcount
          cls  = conann.Classify(dist)                   // SafeIn/Out/Uncertain
          if cls != SafeOut:
              addr = segment.GetAddress(cluster_id, i)   // in-memory lookup
              generate ReadTask → push to queue
    问题：LoadCode 是逐条 pread！

    当前 LoadCode 每次调用都会 pread .clu 文件读一条 code。这在 Probe 循环里会产生 N_cluster 次系统调用。

    当前 LoadCode 路径:
      ClusterStoreReader::LoadCode()
        → pread(fd, code_buf, code_entry_size, codes_offset + idx * code_entry_size)
        → 每条 128维 → 16 bytes → 极小 pread

    优化路径 A: 批量 LoadCodes
      已有 LoadCodes(cluster_id, indices, out_codes)
      但 indices 仍然是逐条指定

    优化路径 B: EnsureClusterLoaded 时直接 mmap/read 整个 codes region
      在 ClusterData 中缓存整个 codes block
      → GetCode(cluster_id, idx) 变为内存访问
      → zero syscall in probe loop
    权衡 5：Probe 时 codes 是否应该全量加载到内存？


      全量加载 (推荐):
        EnsureClusterLoaded 时: read 整个 codes region → ClusterData.codes_buffer
        GetCodePtr(cluster_id, idx) → codes_buffer.data() + idx * code_entry_size
        + Probe 循环零系统调用
        + 可直接传给 EstimateDistanceBatch
        - 内存: 1M 向量 × 128维 → 1M × 16B = 16MB (可接受)

    逐条 pread:
      + 内存小
      - Probe 循环 N 次 pread → 性能灾难

    mmap codes region:
      + 内存交由 OS 管理
      - 首次访问时 page fault
      - 对 NVMe 可能不如 buffered read
    我注意到你现有的 EnsureClusterLoaded 已经会读 address blocks 和解码地址，但 codes 仍然是 lazy pread。Phase 7 之前（或作为 Phase 7 第一步），需要在 EnsureClusterLoaded 中同时缓存 codes region。

    或者更好的方案：给 ClusterStoreReader 加一个 LoadCodesIntoMemory(cluster_id) 方法，一次性读整个 codes block 到 ClusterData.codes_buffer，然后 GetCodePtr() 直接返回指针。

    权衡 6：EstimateDistanceBatch vs 逐条 Estimate + Classify


    方案 A: 全量 batch
      EstimateDistanceBatch(pq, all_codes, N, distances)
      然后 for i: Classify(distances[i]) → filter

      + SIMD 友好，批量 popcount
      - 计算了所有 SafeOut 的距离（浪费）
      - 分配 float[N] 的 distances 缓冲区

    方案 B: 逐条 estimate + classify + 立即生成 ReadTask
      for i:
        dist = EstimateDistance(pq, code_ptr)
        if Classify(dist) != SafeOut:
          push ReadTask

      + 内存友好（无临时 buffer）
      + SafeOut 不浪费计算... 但 popcount 本身很快
      - 分支预测不友好

    方案 C: mini-batch (推荐)
      for chunk in chunks(all_records, 64):
        EstimateDistanceBatch(pq, chunk_codes, 64, chunk_distances)
        for i in chunk:
          if Classify(chunk_distances[i]) != SafeOut:
            push ReadTask
        // 每 64 条检查一次 ReadQueue 满否

      + 兼顾 SIMD 批量 + 早停 + back-pressure
      + 64 个 float 的临时 buffer 在栈上
    推荐方案 C，block_size=64 与你的 address block 粒度一致。

4. RerankConsumer — 状态机
这是流水线中状态最复杂的组件。每个 CompletedRead 需要按 task_type 走不同路径：


                 CompletedRead
                      │
        ┌─────────────┼─────────────────┐
        ▼             ▼                 ▼
     VEC_ONLY       ALL/FRONT         BACK/PAYLOAD
        │             │                 │
    解析 vec      解析 vec+payload   解析 payload
        │             │                 │
   L2Sqr 精排    TryInsert(TopK)    查找对应的
        │             │              partial result
        │             │                 │
   ┌────┴────┐       done         拼接 payload
   │         │                         │
   in TopK?  out                  TryInsert(TopK)
   │                                   │
   生成 BACK task                     done
   → push Qp
权衡 7：Uncertain 的 BACK task 如何关联到原始 VEC_ONLY 结果？

当 VEC_ONLY 完成后，如果确认在 TopK 内，需要发 BACK task 读 payload。BACK 完成时，需要找回之前计算的精确距离和 VecID。


方案 A: ReadTask 中携带额外字段
  struct ReadTask {
    ...
    float exact_dist;        // VEC_ONLY rerank 后填写
    VecID vec_id;            // 用于 BACK 时 TryInsert
  };

  + 简单，无需外部 map
  - ReadTask 变大（+12 bytes），所有类型的 task 都要付这个开销

方案 B: 外部 pending_map
  std::unordered_map<uint64_t, PendingResult> pending_;
  key = (cluster_id << 32) | local_idx

  VEC_ONLY 完成 → 计算 exact_dist → 存入 pending_[key]
  BACK 完成 → 查 pending_[key] → 拼 payload → TryInsert → 删除 key

  + ReadTask 保持精简
  - 额外的 map 查找开销（但 Uncertain 比例通常 <20%）

方案 C: ResultCollector 支持两阶段插入
  stage1: TryInsertPartial(vec_id, dist)  // 只有距离，无 payload
  stage2: AttachPayload(vec_id, payload)  // 后补 payload

  + 语义清晰
  - ResultCollector 变复杂
推荐方案 A——在 ReadTask 里加几个字段，代码最简单。Uncertain 的 BACK task 在生成时就能填好 exact_dist 和 vec_id。

5. ResultCollector — TopK 收集

class ResultCollector {
    // max-heap: top 是当前最大距离（最差的 top-k 结果）
    // TryInsert: if dist < heap_top → pop top, push new → return true
    // Finalize: pop all → reverse → sorted by distance ascending

    std::priority_queue<SearchResult, vector, std::greater<>> heap_;
    // 或者直接用 std::vector + partial_sort
};
权衡 8：payload 是否放在 TopK 堆里？


方案 A: SearchResult 含 payload (当前 types.h 设计)
  struct SearchResult { VecID id; float distance; map<ColumnID, Datum> payload; };

  + 简单，一次 TryInsert 完事
  - 堆操作涉及 move(payload)，对含 string 的 payload 有分配开销
  - 被淘汰的 result 的 payload 白读了

方案 B: 堆只存 (VecID, distance)，payload 存在 side table
  heap: (vec_id, dist) only — 16 bytes, cache-friendly
  side: vec_id → payload (只有最终 top-k 条目有 payload)

  TryInsert(vec_id, dist) → if inserted, 存 payload 到 side table
  Finalize: merge heap + side table

  + 堆操作轻量
  + 被淘汰的条目无需移动 payload
  - 多一次 lookup

方案 C: 延迟读取 payload
  堆只存 (vec_id, dist, AddressEntry)
  Finalize 后: 对最终 top-k 结果再发一轮 I/O 读 payload

  + Probe+Rerank 阶段完全不碰 payload
  + I/O 量最小（只读最终 K 条的 payload）
  - 多一轮 I/O
  - 不适合流式返回
这个选择影响整个 I/O 策略。如果选方案 C，那么 FRONT/PAYLOAD/BACK 这些 task type 就不需要了，Probe 阶段只生成 VEC_ONLY 任务。ConANN 的 SafeIn/Uncertain 分类仍然有用（SafeOut 直接跳过），但 SafeIn 也只读向量部分。


方案 C 的简化效果:
  ReadTaskType: 只需 VEC_ONLY
  Qv/Qp 双队列: 退化为单队列
  Rerank: 只做 L2Sqr + TryInsert(id, dist)
  最后: TopK 结果发 ALL 读取 → 返回
这是一个重大架构决策，建议你考虑：

方案 A (全流式)	方案 C (延迟 payload)
I/O 总量	多（SafeIn 读完整记录）	少（全程只读 vec，最后读 K 条 payload）
复杂度	高（5 种 task type，状态机）	低（1 种 task type，无状态机）
延迟	更好（流式完成）	最后有一轮 payload I/O
适用场景	需要 payload 的搜索	只需 ID+distance 的搜索
如果你的主要场景是先返回 top-k ID + distance，偶尔才需要 payload，方案 C 是大幅简化。

6. OverlapScheduler — 调度主循环
这是整个流水线的大脑。单线程协作式调度的核心状态机：


enum class Phase { PROBE, IO_SUBMIT, IO_POLL, RERANK, DONE };

class OverlapScheduler {
    // 输入
    IvfIndex& index_;
    SearchContext& ctx_;
    AsyncReader& reader_;

    // 状态
    uint32_t current_probe_cluster_ = 0;  // 正在 probe 的 cluster 索引
    uint32_t current_probe_record_ = 0;   // 当前 cluster 内的记录偏移

    ReadQueue queue_;
    std::deque<CompletedRead> completed_;
    RerankConsumer reranker_;

    // 调度参数
    uint32_t probe_batch_size_ = 64;      // 每轮 probe 多少条记录
    uint32_t io_batch_size_ = 32;         // 每轮提交多少 I/O
    uint32_t rerank_batch_size_ = 16;     // 每轮 rerank 多少条
};
调度循环：


Search(query, top_k, nprobe):
  clusters = FindNearestClusters(query, nprobe)
  for each cluster: EnsureClusterLoaded(cluster)

  while not done:
    // Phase 1: Probe (generate ReadTasks)
    if has_clusters_to_probe && queue.Size() < max_queue_size:
      ProbeNext(probe_batch_size)  // 生成 ReadTasks → push Qv/Qp

    // Phase 2: Submit I/O
    if queue.Size() > 0 && reader.InFlight() < max_inflight:
      tasks = queue.PopBatch(io_batch_size)
      reader.Submit(fd, tasks, buffers, count)

    // Phase 3: Poll completions
    n = reader.Poll(completed_buf, poll_batch_size)
    for i in 0..n: completed_.push_back(completed_buf[i])

    // Phase 4: Rerank
    while !completed_.empty():
      reranker_.Consume(completed_.front())
      completed_.pop_front()

    // Termination
    if all_probed && reader.InFlight() == 0 && completed_.empty():
      done = true

  return result_collector.Finalize()
权衡 9：Probe 粒度 — 逐 cluster vs 逐 mini-batch


粒度 A: 逐 cluster 完成后再切 I/O
  ProbeCluster(cluster_id)  // 一次性处理整个 cluster
  → 可能产生数千个 ReadTasks 才开始 I/O
  - 延迟: 第一个 I/O 提交较晚
  + 简单

粒度 B: 逐 mini-batch（推荐）
  ProbeNext(64)  // 当前 cluster 内继续 64 条
  → 如果 cluster 还有剩余，下轮继续
  + I/O 更早开始，overlap 更好
  + back-pressure 自然（队列满时停 probe）
  - 需要维护 (current_cluster_, current_record_) 状态
推荐粒度 B——与 address block 64 条对齐，每次 probe 64 条后检查队列/I/O 状态。

权衡 10：调度循环的退出条件和饥饿处理

需要特别注意的边界情况：


1. Probe 生成 0 个 ReadTask（全部 SafeOut）
   → 不能卡在等 I/O，需要继续 Probe 下一批

2. io_uring Poll 返回 0（全部 in-flight）
   → 需要 io_uring_wait 而非 busy poll
   → 或者回去继续 Probe

3. 所有 cluster probe 完，但仍有 in-flight I/O
   → 切换到纯 poll + rerank 模式

4. TopK 已满 + 堆顶距离很小 → ConANN 分类全部 SafeOut
   → 可以 early exit 跳过剩余 clusters
权衡 11：early termination（提前终止）


当 TopK 堆已满 (size == K) 且所有剩余 cluster 的
centroid 距离 > heap_top + 2ε 时，剩余 cluster 不可能
产生比当前 top-K 更好的结果 → 提前终止。

这个优化的收益取决于数据分布和 nprobe 大小。
对于 nprobe 小 (≤8) 的场景，提前终止收益有限。
对于 nprobe 大 (≥32) 的场景，收益显著。
建议初期不实现 early termination，作为后续优化项。

7. SearchContext — 查询上下文

struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 8;
    uint32_t max_prefetch_size = 1024 * 1024;  // 1MB
    uint32_t probe_batch_size = 64;
    uint32_t io_batch_size = 32;
    uint32_t io_queue_depth = 64;
    uint32_t max_queue_size = 256;
    bool fetch_payload = true;     // false → 方案C延迟payload
};

class SearchContext {
    const float* query_vec_;
    SearchConfig config_;
    ResultCollector collector_;

    // 统计
    uint32_t total_probed_ = 0;
    uint32_t total_safe_in_ = 0;
    uint32_t total_safe_out_ = 0;
    uint32_t total_uncertain_ = 0;
    uint32_t total_io_submitted_ = 0;
    uint32_t total_reranked_ = 0;
};
8. Warmup — 预热

Warmup 需要做的事:
  1. IvfIndex::Open()  (已有)
     → 加载 centroids, rotation, segment.meta
     → ClusterStoreReader::Open() → 读 global header + lookup table

  2. 可选: 预加载热 cluster 的 codes
     对 top-N 最大的 cluster 做 EnsureClusterLoaded()
     → mlock codes region (防止被 swap)

  3. 可选: madvise(.dat 文件, MADV_RANDOM)
     提示内核 .dat 文件是随机读取模式
这个组件相对简单，可以后做。

9. VecID 映射
你现有的 VecID = uint64_t，但 Probe 阶段得到的是 (cluster_id, local_idx)。需要一个映射方案：


方案 A: 编码到 VecID
  vec_id = (uint64_t(cluster_id) << 32) | local_idx
  + 零开销
  - 限制 cluster_id 和 local_idx 各 32bit（足够）

方案 B: 构建时存储映射表
  IvfBuilder 在构建时写入 global_id → (cluster_id, local_idx) 映射
  + 语义干净
  - 多一份数据

方案 C: 构建时写入 original VecID 到 DataFile
  每条记录存储其原始 VecID
  Rerank 阶段从 DataFile 读出
  + 支持非连续 VecID
  - I/O 多读 8 bytes/record
推荐方案 A——简单高效，32bit cluster_id + 32bit local_idx 支持 ~40亿 × ~40亿 个向量。

总结：需要你决定的关键权衡
#	决策	选项	推荐
1	O_DIRECT	初期 buffered / 初期 O_DIRECT	初期 buffered
5	codes 加载策略	全量到内存 / 逐条 pread	全量到内存
6	Probe 批量策略	全量 batch / 逐条 / mini-batch 64	mini-batch 64
7	BACK task 关联	ReadTask 携带字段 / 外部 map	ReadTask 携带
8	payload 策略	全流式 (5种task) / 延迟payload (1种task)	需你判断
9	Probe 粒度	逐 cluster / 逐 mini-batch	mini-batch
3	PopBatch 混合	Qv 全取 / 按比例	Qv 全取
4	队列容量	无界 / 有界 256	有界 256
其中第 8 条（payload 策略）是影响最大的架构决策——它决定了 ReadTaskType 的复杂度、ReadQueue 是否需要双队列、以及 RerankConsumer 的状态机复杂度。选延迟 payload 可以把整个流水线简化一半，代价是最后多一轮 K 次读取。