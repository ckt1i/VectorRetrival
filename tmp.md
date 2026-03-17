原始数据读取的时候，是等payload统一读取完再将原始数据统一返回内存，还是读多少返回多少？好的，理解了你的澄清。Uncertain 的 payload 读取是批量延迟的——全部 Rerank 结束后，只对最终在 TopK 中的 Uncertain 条目统一读取。这大幅简化了设计。

让我画出完整流水线。

完整流水线设计
架构总览

SearchRequest(query_vec, top_k, nprobe)
  │
  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  OverlapScheduler — 交错式主循环                                     │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  主循环: while (有cluster未probe || 队列非空 || InFlight>0)  │   │
│  │                                                             │   │
│  │  ┌──────────┐    ┌──────────┐    ┌──────────┐              │   │
│  │  │ 1. Probe │───▶│ 2. I/O   │───▶│ 3.Rerank │──┐          │   │
│  │  │ NextBatch│    │ Submit   │    │ Process  │  │          │   │
│  │  └──────────┘    └──────────┘    └──────────┘  │          │   │
│  │       ▲                                         │          │   │
│  │       └─────────────────────────────────────────┘          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼ 主循环结束                           │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 2: 补读 Uncertain payload                             │   │
│  │  TopK 中 payload=nullptr 的条目 → 批量 PAYLOAD 读取          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                              │                                     │
│                              ▼                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Phase 3: 组装 SearchResult[]                                │   │
│  │  从 payload_cache 查找 → 填入指针 → 返回                     │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
数据流详图

═══ 主循环（交错执行）══════════════════════════════════════════

  每轮迭代:

  Step 1: Probe 一个批次 (64条 或 一个小cluster)
  ┌──────────────────────────────────────────────────────────┐
  │  EstimateDistanceBatch → Classify                        │
  │                                                          │
  │  SafeOut ──→ skip                                        │
  │                                                          │
  │  SafeIn ───→ VEC_ONLY{addr, dim*4}       ──→ Push Qv    │
  │             PAYLOAD{addr, offset=dim*4}  ──→ Push Qp    │
  │                                                          │
  │  Uncertain → VEC_ONLY{addr, dim*4}       ──→ Push Qv    │
  └──────────────────────────────────────────────────────────┘
                    │                    │
                    ▼                    ▼
              ┌──────────┐        ┌──────────┐
              │    Qv    │        │    Qp    │
              │ (高优先)  │        │ (低优先)  │
              │ 向量读取  │        │ payload  │
              │ VEC_ONLY │        │ PAYLOAD  │
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
  Step 4: 处理完成项
  ┌──────────────────────────────────────────────────────────┐
  │                                                          │
  │  VEC_ONLY 完成:                                          │
  │    parse vec → L2Sqr(query, vec, dim) → exact_dist       │
  │    collector.TryInsert(exact_dist, addr)                  │
  │    release buffer → pool                                 │
  │                                                          │
  │  PAYLOAD 完成 (SafeIn 的 payload):                       │
  │    payload_cache[addr.offset] = buffer                   │
  │    (不释放 buffer, 转移所有权到 cache)                     │
  │                                                          │
  └──────────────────────────────────────────────────────────┘


═══ Phase 2: 补读 Uncertain payload ═════════════════════════

  results = collector.Finalize()
  // results: [(dist, addr), ...] 按 distance 升序

  missing = []
  for r in results:
    if payload_cache.has(r.addr.offset):
      // SafeIn, payload 已在 cache 中 ✓
      continue
    else:
      // Uncertain, 需要补读 payload
      missing.append(r)

  // 批量提交
  for m in missing:
    Submit PAYLOAD{m.addr, offset=dim*4, length=addr.size-dim*4}
  
  // 等待全部完成
  while InFlight() > 0:
    WaitAndPoll → payload_cache[addr.offset] = buffer


═══ Phase 3: 组装最终结果 ═══════════════════════════════════

  final_results = []
  for r in results:
    buf = payload_cache[r.addr.offset]
    payload = ParsePayload(buf)
    final_results.append(SearchResult{
      distance = r.distance,
      payload = payload    // Datum* 指向 cache 中的数据
    })

  // 释放未命中 TopK 的 SafeIn payload cache
  cleanup_unused_cache()

  return final_results
I/O 时序图

假设 nprobe=4, 每cluster ~200条, dim=128

时间 ─────────────────────────────────────────────────────────────────►

CPU:   [Probe C0] [Probe C1]  [Probe C2]  [Probe C3]
        ↓↓↓        ↓↓↓         ↓↓↓         ↓↓↓
Qv:    VEC×80     VEC×60      VEC×90      VEC×70
Qp:    PAY×30     PAY×25      PAY×35      PAY×28    (SafeIn payloads)

I/O:   [──Qv C0──][──Qv C1──][──Qv C1+C2──][──Qv C3──][──Qp──][Qp]
                ↓           ↓              ↓          ↓
Rerank:   [L2Sqr C0]  [L2Sqr C1]    [L2Sqr C2]  [L2Sqr C3]

                                                          Phase 2:
                                                    [补读 Uncertain payload]
                                                          ↓
返回:                                                 [组装 SearchResult]

关键: CPU Probe 和 I/O 读取深度重叠
      Rerank 和下一批 Qv/Qp 读取深度重叠
      SafeIn payload 在 Rerank 期间就已经在读了
调度伪代码

Search(query):
  clusters = FindNearestClusters(query, nprobe)
  for c in clusters: EnsureClusterLoaded(c)
  ctx = SearchContext(query, config)

  // ═══ 交错主循环 ═══
  cluster_idx = 0
  record_offset = 0

  while true:
    // ── Step 1: Probe ──
    if cluster_idx < nprobe:
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

    // ── Step 2: Submit I/O ──
    can_submit = io_queue_depth - reader.InFlight()
    if can_submit > 0 && !queue.Empty():
      tasks = queue.PopBatch(can_submit)   // Qv 优先
      buffers = pool.AcquireBatch(tasks)
      reader.Submit(tasks_as_io_requests)

    // ── Step 3: Poll ──
    n = reader.Poll(completions, 32)
    if n == 0 && reader.InFlight() > 0 && cluster_idx >= nprobe:
      // 没有更多 Probe 工作，阻塞等 I/O
      n = reader.WaitAndPoll(completions, 32)

    // ── Step 4: Process completions ──
    for i in 0..n:
      if completions[i].type == VEC_ONLY:
        vec = parse_vector(completions[i].buffer)
        dist = L2Sqr(query, vec, dim)
        collector.TryInsert(dist, completions[i].addr)
        pool.Release(completions[i].buffer)
      
      elif completions[i].type == PAYLOAD:
        payload_cache[completions[i].addr.offset] = completions[i].buffer
        // buffer 所有权转移, 不释放

    // ── 终止条件 ──
    if cluster_idx >= nprobe && queue.Empty() && reader.InFlight() == 0:
      break

  // ═══ Phase 2: 补读 Uncertain payload ═══
  results = collector.Finalize()
  for r in results:
    if !payload_cache.has(r.addr.offset):
      buf = pool.Acquire(r.addr.size - dim * sizeof(float))
      reader.Submit(PAYLOAD{fd, r.addr.offset + dim*4, payload_len, buf})
  
  while reader.InFlight() > 0:
    WaitAndPoll → payload_cache[addr.offset] = buffer

  // ═══ Phase 3: 组装 ═══
  return AssembleResults(results, payload_cache)
Probe 内部流程

ProbeClusterBatch(ctx, cluster_id, offset, count):
  code_ptrs = GetCodePtrs(cluster_id, offset, count)
  EstimateDistanceBatch(pq, code_ptrs, count, distances)

  for i in 0..count:
    cls = conann.Classify(distances[i])
    
    if cls == SafeOut:
      ctx.stats.total_safe_out++
      continue

    addr = segment.GetAddress(cluster_id, offset + i)

    // 所有非 SafeOut: 向量读取 → Qv
    queue.PushQv(ReadTask{
      type = VEC_ONLY,
      addr = addr,
      read_offset = addr.offset,
      read_length = dim * sizeof(float),
    })

    if cls == SafeIn:
      ctx.stats.total_safe_in++
      // SafeIn: 额外提交 payload 读取 → Qp
      queue.PushQp(ReadTask{
        type = PAYLOAD,
        addr = addr,
        read_offset = addr.offset + dim * sizeof(float),
        read_length = addr.size - dim * sizeof(float),
      })
    else:  // Uncertain
      ctx.stats.total_uncertain++

  ctx.stats.total_probed += count
关键数据结构

// ResultCollector — 无 VecID
struct CollectorEntry {
    float distance;
    AddressEntry addr;
    bool operator<(const CollectorEntry& o) const {
        return distance < o.distance;  // max-heap
    }
};

class ResultCollector {
    uint32_t top_k_;
    std::priority_queue<CollectorEntry> heap_;  // max-heap
public:
    bool TryInsert(float dist, AddressEntry addr);
    float TopDistance() const;
    bool Full() const;
    std::vector<CollectorEntry> Finalize();  // sorted by dist asc
};

// Payload 缓存
std::unordered_map<uint64_t, std::unique_ptr<uint8_t[]>> payload_cache_;
// key = addr.offset (每条 record 的偏移唯一)

// 最终返回给用户
struct SearchResult {
    float distance;
    Datum* payload;  // 指向 payload_cache 中的解析数据
};
Buffer 生命周期

┌──────────────────────────────────────────────────────────────┐
│  Buffer 生命周期                                              │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  VEC_ONLY buffer:                                            │
│    pool.Acquire() → Submit → Complete → L2Sqr → Release()    │
│    生命周期: 一次 I/O 完成后立即回收                            │
│                                                              │
│  PAYLOAD buffer (SafeIn, Probe 阶段产生):                    │
│    pool.Acquire() → Submit → Complete → payload_cache        │
│    生命周期: 转移到 cache, 搜索结束后统一释放                   │
│                                                              │
│  PAYLOAD buffer (Uncertain, Phase 2 产生):                   │
│    pool.Acquire() → Submit → Complete → payload_cache        │
│    生命周期: 同上                                             │
│                                                              │
│  搜索结束后:                                                  │
│    用户持有 SearchResult[] 中的 Datum* 指针                    │
│    指向 payload_cache 中的数据                                 │
│    → payload_cache 必须活到用户用完为止                        │
│    → 由 SearchContext 或 Scheduler 持有 cache 所有权           │
│                                                              │
└──────────────────────────────────────────────────────────────┘
需要变更的设计决策
#	决策项	原决策	新决策	影响
1	调度模型	模式 Z（Probe 全完 → I/O+Rerank）	交错模式（Probe/I/O/Rerank 每轮迭代交替）	Scheduler 主循环重写
2	Payload 策略	延迟 payload（方案 C），全程 VEC_ONLY	混合：SafeIn prefetch PAYLOAD → Qp；Uncertain 延迟到 Rerank 后批量读	ReadTask 从 1 种变 2 种，新增 payload_cache
3	ReadTaskType	只用 VEC_ONLY	VEC_ONLY + PAYLOAD	完成项处理需按类型分支
4	Qv/Qp 双队列	退化为单队列	真正的双队列：Qv=向量，Qp=payload，Qv 优先	ReadQueue 功能真正启用
5	VecID	合成编码 (cluster<<32|idx)	移除，不返回 ID	删除 EncodeVecID/DecodeVecID
6	SearchResult	{VecID, float, map<ColumnID,Datum>}	{float distance, Datum* payload}	接口变更
7	ResultCollector	存 (VecID, dist, AddressEntry)	存 (dist, AddressEntry)	堆元素更小
8	RerankConsumer	只处理 VEC_ONLY	按 type 分支：VEC_ONLY → Rerank，PAYLOAD → cache	逻辑变复杂
9	Buffer 生命周期	统一 acquire/release	VEC_ONLY → release，PAYLOAD → 转移到 cache	新增 payload_cache 管理
10	队列 back-pressure	模式 Z 不需要	需要：Qv 满时暂停 Probe，让 I/O 先消耗	Scheduler 需检查队列容量
有几个细节需要你确认：

Datum* payload 的生命周期：用户拿到 SearchResult[] 后，谁负责释放底层 buffer？是用户手动调用 ReleaseResults()，还是 SearchResult 用 RAII 管理？

queue back-pressure 阈值：Qv 满到多少时暂停 Probe？还是用之前的 256？

是否要记录到 design.md 和 tradeoffs 文件？这是一次比较大的设计方向调整。

