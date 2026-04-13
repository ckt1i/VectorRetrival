## Context

当前系统的热路径瓶颈已经从计算侧转移到 `io_uring` 提交侧。最新 benchmark 显示，COCO 100K、`nprobe=200`、warm steady-state 下，平均总延迟约 3.50ms，其中：

- `uring_submit_ms = 1.86ms`，约占 53.1%
- `probe_time_ms = 0.87ms`
- `uring_prep_ms = 0.31ms`
- `io_wait_ms = 0.008ms`

这意味着当前的主要问题不是设备等待，而是用户态向内核提交 SQE 的组织方式。代码现状包括：

- `DEFER_TASKRUN + SINGLE_ISSUER` 已在 `IoUringReader::Init()` 启用
- fixed files 已通过 `RegisterFiles()` 接入 benchmark 初始化
- `submit_batch_size` 已存在，但由于 SQ depth 固定为 64，且 cluster refill 与 vec reads 分开提交，批量收益经常被 auto-flush 抵消
- cluster block 读取与 vec/payload 读取共用单 ring，且 cluster refill 当前采用立即 submit 策略，会打断 vec side batching
- completion dispatch 仍通过 `pending_` 哈希表按 buffer 指针查找

这次设计的目标不是改变召回或分类逻辑，而是降低 submit-side CPU 成本，并建立一套可验证的 benchmark 控制面，判断更深 SQ、SQPOLL、双 ring 是否值得长期保留。

## Goals / Non-Goals

**Goals:**
- 将 submit-side 优化拆成可渐进实施的两阶段方案：单 ring 收敛优先，双 ring / SQPOLL 作为后续增强
- 让 `io_queue_depth` 真正接入 ring 初始化，并在 benchmark 中参数化
- 将 cluster refill submit 与 vec/payload submit 统一到更少的提交点，减少 `io_uring_enter` 次数
- 支持可选 SQPOLL 模式，并通过 benchmark 明确其在 warm-path 下的收益
- 为 cluster read 与 vec/payload read 定义隔离策略，至少支持“逻辑配额隔离”，可选扩展到双 ring
- 降低 CQE completion dispatch 的常数开销，为后续更深队列做准备
- 提供清晰的验收指标，重点观察 `uring_submit_ms`、总延迟和提交次数

**Non-Goals:**
- 不改变搜索结果排序、SafeOut / SafeIn / Uncertain 判定或 CRC 逻辑
- 不引入多线程查询调度或 fiber/coroutine 架构
- 不将 O_DIRECT / IOPOLL 作为本次主线；这些仍属于 cold-data / data>>RAM 场景优化
- 不承诺一次性落地所有微优化；slot vector、fixed buffers 作为第二层优化

## Decisions

### Decision 1: 先做单 ring submit-path 收敛，再评估双 ring

第一阶段保持单 ring 架构，但改变提交组织方式：

- 在一次调度迭代中，优先累计 vec/payload SQE 与 refill cluster SQE
- 将原本分散的 `flush_vec_reads()` 与 `flush_cluster_reads()` 收敛到统一提交节点
- 在“马上要等待 cluster completion”之前，执行一次统一 flush，必要时使用 `submit_and_wait` 语义来合并一次内核进入

理由：

- 当前瓶颈首先是“进入内核太频繁”，而不是“单 ring 本身绝对不够用”
- 单 ring 改造风险更低，且能更快验证 submit 次数减少后是否真的转化为 wall-time 收益
- 若单 ring 已把 `uring_submit_ms` 压到 <1ms，再决定是否需要双 ring 更稳妥

备选方案：

- 直接上双 ring。优点是 cluster read 与 vec read 完全隔离；缺点是实现与 benchmark 复杂度更高，且在未验证单 ring 收敛收益之前，容易把问题扩大化。

### Decision 2: 将 SQ depth 变成真正生效的运行参数

第二个优先决策是把 `SearchConfig::io_queue_depth` 与 benchmark CLI 连接到 `IoUringReader::Init()`，不再在 benchmark 中硬编码 `64`。

理由：

- 当前 batching 收益被抵消的重要原因之一，是 SQ depth 太小，`PrepRead()` 一旦拿不到 SQE 就立刻 auto-flush
- 不提高 SQ depth，就无法公平评估 submit batching、统一 submit 节点和 SQPOLL 的真实收益
- 更深的 SQ depth 也是双 ring 之前必须先掌握的基本调参面

备选方案：

- 保持 SQ depth=64，仅优化提交点。这样可以省去一层参数化工作，但会让很多 batching 优化被队列容量上限掩盖，容易得出错误结论。

### Decision 3: 将 SQPOLL 作为可选增强，而非默认行为

设计中引入可选 `use_sqpoll` 路径，但默认不开启。benchmark 通过显式参数控制是否启用，并记录与非 SQPOLL 的对比。

理由：

- 现在最明确的瓶颈是 submit syscall，本质上更匹配 SQPOLL 的问题形态
- 但 SQPOLL 会引入专属内核 poll 线程，适合通过 benchmark 证明收益后再决定默认策略
- 在某些低负载配置下，SQPOLL 唤醒路径可能收益有限，甚至不如显式 submit 稳定

备选方案：

- 直接启用 SQPOLL 作为默认路径。优点是可能最快拿到 submit-side 收益；缺点是默认资源占用变化较大，不利于先做可控对比。

### Decision 4: 将 ring 隔离设计成“两级方案”

本次设计把隔离策略分为两个层级：

- Level 1：单 ring + 逻辑配额隔离。对 cluster refill 与 vec/payload reads 设定独立的提交阈值或保留槽位，避免 cluster submit 挤爆 vec batching。
- Level 2：双 ring。cluster ring 负责 `.clu` block 预取，data ring 负责 `.dat` vec/payload reads，CQE 在调度器中统一消费。

理由：

- Level 1 改动小，能先验证“互相挤占”是不是主要问题
- Level 2 才是彻底的资源隔离，但需要更多 reader/scheduler 生命周期管理
- 这种分层让实现顺序和风险控制都更清晰

备选方案：

- 只保留双 ring 方案。这样概念更干净，但会跳过一个低成本、足以解释问题来源的中间层。

### Decision 5: completion dispatch 改为 slot-id 驱动

在更深 SQ 和更高 batch 下，`pending_` 的哈希表查找会越来越频繁。本次设计引入 slot vector + freelist 方向，并在 `user_data` 中直接编码 slot index。

理由：

- 这是小改动、低风险的热路径常数优化
- 更适合作为 submit-side 主优化完成后的第二层收尾项
- 为双 ring 或更深队列准备更稳定的 completion bookkeeping

备选方案：

- 保持哈希表。实现最简单，但在更深队列与高 batch 下会让 CQE 消费路径承担不必要的查找开销。

## Risks / Trade-offs

- [Risk] 统一 submit 节点后，若 flush 时机过晚，可能拉长 cluster ready 时间
  → Mitigation: 保留“等待前强制 flush”语义，并通过 benchmark 比较 cluster-ready stall 与 submit 次数

- [Risk] 提高 SQ depth 可能带来更高内存占用和更长的单次 drain 批量
  → Mitigation: 在 benchmark 中限定候选值（如 64/128/256/512），并持续记录延迟而非只看 submit 时间

- [Risk] SQPOLL 在低负载或短查询下收益不稳定
  → Mitigation: 作为可选配置引入，先用 benchmark 做对照，不作为默认路径

- [Risk] 双 ring 会增加调度器复杂度，尤其是两类 CQE 的 drain 和统计
  → Mitigation: 先做 Level 1 逻辑隔离，仅在单 ring 收益不足时推进双 ring

- [Risk] slot-id completion dispatch 使 buffer 生命周期和 metadata 生命周期耦合更紧
  → Mitigation: 用 freelist 显式回收 slot，并在 query 结束时保留 orphan slot 清理逻辑

- [Trade-off] 本次聚焦 warm-path submit-side，会推迟 O_DIRECT / IOPOLL 的主线推进
  → 这是有意取舍，因为当前数据已经明确表明 warm-path 主要不是设备等待问题

## Migration Plan

1. 在 benchmark 中接通 `io_queue_depth`、`use_sqpoll` 和 ring 隔离相关配置项
2. 先实现单 ring 提交点收敛与更深 SQ depth 验证
3. 若 `uring_submit_ms` 仍显著高于 1ms，再推进 Level 1 配额隔离与 Level 2 双 ring
4. 在主收益项稳定后，再加入 slot vector / fixed buffer 等微优化
5. 保留原单 ring、非 SQPOLL 路径作为 fallback，确保可以快速回退

## Open Questions

- warm-path 下，单 ring 收敛后 `uring_submit_ms` 是否已经足够低，以至于双 ring 不再值得？
- 在当前工作负载里，SQPOLL 的收益主要来自减少 syscall，还是也改变了 CQ 提交/消费节奏？
- slot vector 的收益是否只有在更深 SQ depth 下才会显著，还是在现有规模下也足够值得？

## Validation Results (2026-04-13)

验证 workload 固定为：

- dataset: `COCO 100K`
- index: 2026-04-13 新构建的当前格式索引，`nlist=2048`，`bits=4`
- query protocol: `nprobe=200`，`top_k=10`，`queries=500`
- warm-path protocol: 使用 `bench_e2e --nprobe-sweep 200`，即 `100` 条 warmup + `500` 条 measurement
- index path: `/home/zcq/VDB/test/io_submit_validation_build/coco_100k_20260413T151640/index`

### Validation Table

| Mode | Queue depth | SQPOLL effective | avg(ms) | p99(ms) | probe(ms) | uring_submit(ms) | submit_calls | recall@10 |
|------|-------------|------------------|---------|---------|-----------|------------------|--------------|-----------|
| shared | 64  | 0 | 1.0817 | 1.9396 | 0.4272 | 0.4456 | 23.04 | 0.8984 |
| shared | 128 | 0 | 1.1068 | 1.9974 | 0.4172 | 0.4635 | 23.06 | 0.8984 |
| shared | 256 | 0 | 1.1201 | 2.0038 | 0.4235 | 0.4689 | 23.06 | 0.8984 |
| shared | 512 | 0 | 1.1073 | 1.9804 | 0.4169 | 0.4632 | 23.06 | 0.8984 |
| shared + requested SQPOLL | 64 | 0 | 1.0981 | 1.9622 | 0.4241 | 0.4475 | 23.04 | 0.8984 |
| isolated | 64 | 0 | 1.1066 | 1.9929 | 0.4245 | 0.4552 | 24.48 | 0.8984 |

### Validation Conclusions

- `queue depth=64` 已经是该 workload 下的最优点；继续加深到 `128/256/512` 没有带来额外收益，反而让 `avg_query_ms` 增加约 `2.3%/3.6%/2.4%`。
- shared 模式下，`uring_submit_ms` 已稳定在 `0.445-0.469ms`，显著低于设计阶段的历史基线 `1.86ms`。相同 `nprobe=200` workload 下，submit-side 时间下降约 `76%`，总延迟从约 `3.50ms` 降到 `1.08ms`，已超过 proposal 中的积极目标区间。
- 这台机器上请求 `SQPOLL` 后 `effective` 仍为 `0`，说明初始化发生了兼容性回退；因此本次不能把 `SQPOLL` 收益作为已验证结论，只能保留为可选路径。
- isolated mode 在同一 workload 下没有优于 shared mode：总延迟约慢 `2.3%`，`submit_calls` 从 `23.04` 升到 `24.48`。在单 ring 收敛后，双 ring 更像保留型 fallback，而不是默认推荐路径。
- slot vector/freelist 与 registered fixed-vec slab 的实现达成了“更深队列下可扩展”的目标，但从 sweep 结果看，它们属于微优化收尾项，主要价值是把 submit/completion 热路径常数项压低并稳定在 `~0.45ms` 区间，而不是再单独带来新的数量级收益。
