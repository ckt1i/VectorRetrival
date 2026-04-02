# IO 优化完整方案

> **创建日期**：2026-04-02
> **目标硬件**：Kioxia KCD61LUL3T84 NVMe（4KB 随机读 1M IOPS，顺序读 ~6.9 GB/s）
> **数据规模**：100K ~ 100M 向量，nlist = 4√N
> **当前状态**：单线程 io_uring 查询流水线，100% CPU-bound（io_wait < 0.02ms / 126ms 查询）

---

## 目录

1. [背景与现状分析](#1-背景与现状分析)
2. [io_uring 引擎层优化](#2-io_uring-引擎层优化)
3. [文件访问层优化](#3-文件访问层优化)
4. [提交与完成策略优化](#4-提交与完成策略优化)
5. [内存分配优化](#5-内存分配优化)
6. [IO 路径架构优化](#6-io-路径架构优化)
7. [IO 粒度与硬件适配](#7-io-粒度与硬件适配)
8. [全局总览表](#8-全局总览表)
9. [依赖关系图](#9-依赖关系图)
10. [推荐实施路径](#10-推荐实施路径)

---

## 1. 背景与现状分析

### 1.1 当前 IO 架构

```
                          .clu 文件
┌─────────────────────────────────────────────────────┐
│  Global Header + Lookup Table                       │  ← Open() 时 pread 一次性读入内存
│  (centroids, block_offset, block_size, epsilon...)  │
├─────────────────────────────────────────────────────┤
│  Cluster Block [0]   (FastScan + ExRaBitQ + Addr)   │  ← 查询时 io_uring 按需读取
│  Cluster Block [1]                                  │
│  ...                                                │
│  Cluster Block [N-1]                                │
└─────────────────────────────────────────────────────┘

                          .dat 文件
┌─────────────────────────────────────────────────────┐
│  Record[0]: [raw_vector | payload...]               │  ← 查询时 io_uring 按需读取
│  Record[1]: [raw_vector | payload...]               │     (SafeIn/Uncertain 分类后)
│  ...                                                │
└─────────────────────────────────────────────────────┘
```

查询 IO 路径：

```
FindNearestClusters (纯内存)
    │
    ▼
PrefetchClusters ──→ io_uring PrepRead × 16 (cluster blocks from .clu)
    │
    ▼
ProbeAndDrainInterleaved
    │
    ├── WaitAndPoll CQE
    │     ├── CLUSTER_BLOCK 完成 → ParseClusterBlock → ProbeCluster
    │     │                          │
    │     │                          ├── SafeIn  → PrepRead(.dat, vec+payload)
    │     │                          ├── Uncertain → PrepRead(.dat, vec_only)
    │     │                          └── SafeOut → skip
    │     │
    │     ├── VEC_ONLY 完成 → Rerank
    │     └── VEC_ALL 完成 → Consume
    │
    └── Refill sliding window (submit more cluster blocks)
```

### 1.2 Benchmark 数据（100K, nlist=256, dim=512, bits=4）

```
┌──────────────────────────────────────────────────────┐
│  Cluster Block 特征:                                  │
│    avg=419KB, median=388KB, max=1.1MB, min=4.2KB     │
│    Offset 对齐: 256/256 均不满足 4KB 对齐             │
│                                                      │
│  Block size 分布:                                    │
│     <128KB:   8  clusters  (3%)                      │
│  128-512KB: 188  clusters (73%)  ← 主体              │
│  512KB-1MB:  58  clusters (23%)                      │
│     >1MB:     2  clusters (1%)                       │
│                                                      │
│  查询性能 (本机, page cache):                         │
│    avg_query:      125.8ms                           │
│    avg_io_wait:      0.02ms (0.016%)                 │
│    avg_probe:      110.0ms                           │
│    S1 SafeOut:      55.9%                            │
│    S2 SafeOut:      44.0%                            │
│    Uncertain:        0.14% (~141 vecs → 282KB .dat)  │
│    overlap_ratio:    0.9998                          │
└──────────────────────────────────────────────────────┘
```

### 1.3 基于 nlist = 4√N 的全规模投影

| N | nlist (4√N) | vecs/cluster | block size (avg) | .clu total | 典型 nprobe (~√nlist) |
|---|---|---|---|---|---|
| 100K | 1,265 | 79 | ~88 KB | ~108 MB | ~36 |
| 1M | 4,000 | 250 | ~278 KB | ~1.06 GB | ~63 |
| 10M | 12,649 | 791 | ~880 KB | ~10.9 GB | ~112 |
| 100M | 40,000 | 2,500 | ~2.7 MB | ~105 GB | ~200 |

每次查询的 IO 量与 NVMe 预估延迟：

| N | cluster IO (nprobe×blk) | .dat IO (~0.14%×probed) | NVMe cluster IO time | NVMe .dat IO time |
|---|---|---|---|---|
| 100K | 3.1 MB | ~250 KB | 0.45 ms | 0.03 ms |
| 1M | 17.5 MB | ~870 KB | 2.5 ms | 0.09 ms |
| 10M | 98.6 MB | ~4.9 MB | 14.3 ms | 0.49 ms |
| 100M | 540 MB | ~27.5 MB | 78.3 ms | 2.75 ms |

### 1.4 核心判断

在 `nlist = 4√N` 策略下，IO/CPU 比率始终 ≈ 1%，**单线程查询在所有规模下几乎都是 CPU-bound**。IO 优化的目标不是"更快读"，而是：

1. **确保 IO 永远不成为瓶颈**——即使数据集远超 RAM
2. **减少 p99 毛刺**——DEFER_TASKRUN 的核心价值
3. **为未来多线程查询做好准备**

### 1.5 io_uring vs mmap 决策

**结论：不换 mmap，保持 io_uring**。

| 维度 | io_uring | mmap |
|------|----------|------|
| 首次访问延迟 | 可控：显式预取 + batch submit | 不可控：page fault |
| 预取控制 | 精确：prefetch_depth 可调 | 粗糙：依赖内核 readahead |
| 访问模式匹配 | 完美适配随机读取 | 随机读取时 page fault 开销大 |
| 内存管理 | 用户态 buffer，精确控制生命周期 | 内核 LRU，无法精确控制 |
| IO-计算重叠 | 天然支持 submit→probe→wait 流水线 | 需要 madvise + 额外线程 |
| 与 O_DIRECT | 天然兼容 | 互斥 |

核心原因：cluster block 访问模式是随机的（按距离排序，物理位置不连续），mmap readahead 无效；且当前已有精确的 IO-计算重叠，mmap 下 page fault 会阻塞线程。

---

## 2. io_uring 引擎层优化

### 2.1 DEFER_TASKRUN + SINGLE_ISSUER

**涉及文件**：`src/query/io_uring_reader.cpp`

```
当前代码 (io_uring_reader.cpp:26-28):
  params.flags = IORING_SETUP_CQSIZE;

改为:
  params.flags = IORING_SETUP_CQSIZE
               | IORING_SETUP_SINGLE_ISSUER
               | IORING_SETUP_DEFER_TASKRUN;
```

**原理**：

```
当前 (无 DEFER_TASKRUN):
  ┌───────────────────────────────────────────────────────┐
  │  用户线程:  [──── ProbeCluster CPU 计算 ────]         │
  │                        ↑                              │
  │  内核:     CQE 就绪时立即执行 task_work               │
  │            → 抢占用户线程 → context switch             │
  │            → probe 计算被打断 → cache line 被污染      │
  └───────────────────────────────────────────────────────┘

启用后:
  ┌───────────────────────────────────────────────────────┐
  │  用户线程:  [──── ProbeCluster CPU 计算 ────][wait]   │
  │                                              ↑        │
  │  内核:     task_work 延迟到 wait 时才执行     │        │
  │            → 无抢占, CPU 计算不被打断          │        │
  └───────────────────────────────────────────────────────┘
```

| 维度 | 分析 |
|------|------|
| **代码改动** | 3 行，仅 Init() |
| **收益** | p99 延迟更稳定（当前 p99-p50 = 3.9ms，内核抢占是主要来源之一） |
| **前置条件** | 内核 ≥ 5.19；DEFER_TASKRUN 要求 SINGLE_ISSUER |
| **风险** | 老内核不支持（需 fallback）；SINGLE_ISSUER 限制该 ring 只能从创建线程操作 |
| **权衡** | 当前单线程模型完美匹配 SINGLE_ISSUER；未来多线程查询需每线程一个 ring |

### 2.2 IOPOLL（硬件轮询完成）

```
params.flags |= IORING_SETUP_IOPOLL;
```

原理：完成侧改为轮询而非中断，减少 NVMe 硬件中断开销。

| 维度 | 分析 |
|------|------|
| **代码改动** | 1 行 flag |
| **收益** | 每个 CQE 完成省 ~1-2μs 中断处理开销 |
| **前置条件** | **必须 O_DIRECT**（IOPOLL 不支持 buffered IO） |
| **风险** | WaitAndPoll 变为 busy-wait，消耗 CPU 周期 |
| **权衡** | 与当前 CPU-bound 特征矛盾——busy-wait 消耗的 CPU 可能比省下的中断开销更大。适合 IO-bound 场景（数据集远超 RAM）。建议仅在 NVMe 上实测验证 |

### 2.3 SQPOLL（内核轮询提交）

```
params.flags |= IORING_SETUP_SQPOLL;
params.sq_thread_idle = 2000;  // 2ms idle 后休眠
```

原理：内核创建专属线程轮询 SQ，用户态填 SQE 后无需 io_uring_submit() 系统调用。

| 维度 | 分析 |
|------|------|
| **代码改动** | ~10 行（Init flag + submit 逻辑调整） |
| **收益** | 每次 Submit() 省去一次 `io_uring_enter` 系统调用（~1-2μs）|
| **前置条件** | 内核 ≥ 5.10（稳定）；可能需要 `CAP_SYS_NICE` 或 root |
| **风险** | sq_thread 占用一个 CPU 核心的 0.5-1%；低并发时 thread 休眠/唤醒有额外延迟 |
| **权衡** | 每 query 约 258 次 Submit，258 × 2μs = 0.5ms，占总查询时间 0.4%。除非查询延迟要求极端（<1ms），投入产出比低 |

### 2.4 注册用户态缓冲区 (Fixed Buffers)

```
初始化:
  struct iovec iovecs[N];
  for (int i = 0; i < N; i++) {
      iovecs[i].iov_base = aligned_alloc(4096, buf_size);
      iovecs[i].iov_len = buf_size;
  }
  io_uring_register_buffers(&ring, iovecs, N);

使用:
  io_uring_prep_read_fixed(sqe, fd, buf, len, offset, buf_index);
```

原理：预先注册 buffer 地址，内核跳过 get_user_pages + 页表查找，DMA 直传。

| 维度 | 分析 |
|------|------|
| **代码改动** | ~80 行（改 BufferPool + AsyncReader 接口 + IoUringReader） |
| **收益** | 消除每次 PrepRead 的 `get_user_pages` + 页表查找（~0.5-1μs/次）|
| **前置条件** | buffer 数量和大小需预先确定；需提升 RLIMIT_MEMLOCK |
| **风险** | fixed buffer 数量不够时需 fallback 到普通 PrepRead |
| **权衡** | cluster block 大小可变（4KB~1.1MB），不适合 fixed buffer。vec 读取固定大小（dim×4=2048B），适合 fixed buffer。建议**只注册 vec 读的 buffer pool**：64 × 2048B = 128KB locked memory |

### 2.5 注册文件描述符 (Fixed Files)

```
int fds[2] = {clu_fd, dat_fd};
io_uring_register_files(&ring, fds, 2);

// 使用:
sqe->flags |= IOSQE_FIXED_FILE;
io_uring_prep_read(sqe, fd_index, buf, len, offset);
```

原理：预注册 fd，内核跳过每次 SQE 的 fget()/fput() 引用计数操作。

| 维度 | 分析 |
|------|------|
| **代码改动** | ~15 行 |
| **收益** | 每个 SQE 省去 fget/fput 原子操作（~200ns/SQE）|
| **前置条件** | fd 在注册后不能关闭/重新打开 |
| **风险** | 很小，只有 2 个静态 fd |
| **权衡** | 每 query ~756 SQE × 200ns = 151μs = 0.12%。低成本低收益，可随其他优化顺带实施 |

---

## 3. 文件访问层优化

### 3.1 O_DIRECT（绕过 Page Cache）

**涉及文件**：`src/storage/cluster_store.cpp`、`src/storage/data_file_reader.cpp`

```
当前:
  fd_ = ::open(path.c_str(), O_RDONLY);

优化:
  fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECT);
```

| 维度 | 分析 |
|------|------|
| **代码改动** | 每个 open() 加 O_DIRECT + 配置开关 |
| **收益** | 大数据集（>RAM）避免 page cache thrashing；DMA 直传减少拷贝；IOPOLL 前提 |
| **前置条件** | 全部 IO 的 buffer、offset、length 必须满足对齐要求 |
| **风险** | 对齐不满足时 read 直接失败 (EINVAL)；小数据集反而更慢 |
| **权衡** | 建议通过配置项控制，小数据集用 buffered IO，大数据集开 O_DIRECT |

**建议的分层策略**：

```
if (total_data_size < available_ram * 0.5):
    open(O_RDONLY)             // 小数据集: page cache 有效
    prefetch_depth = 16       // 正常预取 
else:
    open(O_RDONLY | O_DIRECT)  // 大数据集: bypass page cache
    prefetch_depth = max(16, bandwidth_delay_product)         
      // 可能需要更多 inflight IO 来饱和 NVMe 带宽 
```

### 3.2 .clu 文件布局 4KB 对齐

**涉及文件**：`src/storage/cluster_store.cpp`（Writer + Reader）

```
当前布局:
  [Header 285B][Lookup 521KB][Block0 紧接 Block1 紧接 ...]
  offset 不对齐, 256/256 均不满足 4KB 对齐

优化布局:
  [Header 285B][Pad→4KB][Lookup 521KB][Pad→4KB][Block0][Pad→4KB][Block1]...
  所有 block offset 和 size 均 4KB 对齐
```

| 维度 | 分析 |
|------|------|
| **代码改动** | ~50 行改 Writer（BeginCluster 时 pad offset）+ Reader 适配 |
| **收益** | O_DIRECT 的必要前置条件；NVMe 对齐读性能最优 |
| **风险** | 旧版 .clu 文件不兼容（需重建索引或版本兼容处理） |
| **权衡** | 额外空间 ≤ 255 × 4095B ≈ 1MB（<< 104.8MB .clu total，<1%）；read length 需 round_up 到 512B 倍数 |

### 3.3 .dat 文件 O_DIRECT 支持

> **结论：.dat 已天然满足 O_DIRECT 对齐要求，无需额外改动。**

当前 `DataFileWriter` 在 `page_size=4096` 下已经将每条 record pad 到 4KB 边界（`data_file_writer.cpp:86-101`）。`AddressColumn` 用 page index 编码地址（`真实地址 = 解码地址 × 4096`），因此：

```
.dat 文件布局 (page_size=4096):
  ┌──────────────────────────┐  offset = 0×4096 = 0      ✓ 4KB 对齐
  │ vec[0] 2048B + payload   │
  │ + pad → 4096B            │  size = 4096B             ✓ 4KB 倍数
  ├──────────────────────────┤  offset = 1×4096 = 4096   ✓ 4KB 对齐
  │ vec[1] 2048B + payload   │
  │ + pad → 4096B            │  size = 4096B             ✓ 4KB 倍数
  ├──────────────────────────┤
  │ ...                      │
  └──────────────────────────┘

AddressEntry.offset 总是 4096 的倍数 ✓
AddressEntry.size 总是 4096 的倍数   ✓
```

**只需 buffer 分配改为 `aligned_alloc(4096, ...)` 即可对 .dat 开 O_DIRECT**。.clu 和 .dat 可以同时开 O_DIRECT。

### 3.4 Lookup Table 一次性 bulk pread

**涉及文件**：`src/storage/cluster_store.cpp` ClusterStoreReader::Open()

```
当前:
  逐字段 PreadValue 读取 lookup table
  256 个 entry × 每 entry 多次 pread ≈ ~2000 次 pread

优化:
  一次 pread 读取整个 lookup table (256 × 2084B = 521KB)
  然后在内存中 parse
```

| 维度 | 分析 |
|------|------|
| **代码改动** | ~20 行 |
| **收益** | Open() 从 ~2000 次 pread 减少到 1 次，节省 ~4ms 启动时间 |
| **风险** | 极小 |
| **权衡** | 只影响启动/打开时间，不影响查询性能。O_DIRECT 下逐字段小 pread 无法满足对齐，此优化变为必需 |

---

## 4. 提交与完成策略优化

### 4.1 ProbeCluster 内分批 Submit

**涉及文件**：`src/query/overlap_scheduler.cpp` ProbeCluster()

```
当前:
  ProbeCluster 内所有 PrepRead 完成后，回到调用方一次性 Submit

优化:
  for each vector in cluster:
      if (SafeIn || Uncertain):
          PrepRead(...)
          prepped++
          if (prepped >= 16):
              reader_.Submit()  // 分批提交
  reader_.Submit()  // flush 剩余
```

| 维度 | 分析 |
|------|------|
| **代码改动** | ~15 行 |
| **收益** | .dat vec reads 更早进入 NVMe 队列，结合 interleaved drain 更好重叠 |
| **前置条件** | 需要在 ProbeCluster 中获取 prepped 计数 |
| **风险** | 增加 Submit 系统调用次数 |
| **权衡** | **当前 per-cluster Uncertain ≈ 141/256 ≈ 0.55 个，大部分 cluster 0 个 vec read——分批 Submit 在当前参数下几乎无效**。只有当 Uncertain 率上升或 per-cluster 向量数增大时才有意义 |

### 4.2 WaitAndPoll 批量消费优化

**涉及文件**：`src/query/io_uring_reader.cpp` WaitAndPoll()

```
当前:
  io_uring_wait_cqe → 1 个
  while (io_uring_peek_cqe → 逐个) ...

可选:
  io_uring_wait_cqe_nr(&ring, &cqe, min_count)  // 等待至少 N 个
  io_uring_peek_batch_cqe(&ring, cqes, max)       // 批量 peek
```

| 维度 | 分析 |
|------|------|
| **代码改动** | ~10 行 |
| **收益** | 减少 CQE ring 访问次数，批量 seen 减少 memory barrier |
| **前置条件** | liburing ≥ 2.0 |
| **风险** | `wait_cqe_nr` 可能增加等待延迟 |
| **权衡** | 收益极小。当前 io_wait=0.02ms，优化空间几乎为零 |

### 4.3 Sliding Window 参数调优

**涉及文件**：`include/vdb/query/search_context.h` SearchConfig

```
当前:
  prefetch_depth = 16
  initial_prefetch = 4   (CRC 模式)
  refill_threshold = 2
  refill_count = 2
```

**所有规模下 prefetch_depth=16 的分析**：

| 规模 | prefetch IO 量 | NVMe 读完时间 | probe 16 clusters 时间 | 结论 |
|------|---|---|---|---|
| 100K | 16 × 88KB = 1.4MB | 0.2ms | 8.3ms | 绰绰有余 |
| 1M | 16 × 278KB = 4.5MB | 0.65ms | ~8ms | 绰绰有余 |
| 10M | 16 × 880KB = 14MB | 2ms | ~176ms | 极度过剩 |
| 100M | 16 × 2.7MB = 43MB | 6.3ms | ~560ms | 极度过剩 |

| 维度 | 分析 |
|------|------|
| **代码改动** | 0 行（调参）或 ~20 行（自适应） |
| **收益** | CRC early stop 时减少浪费的预读 |
| **权衡** | 当前参数已够用。内存峰值 = prefetch_depth × max_block_size（16 × 1.1MB = 17.6MB for 100K）。可以根据 `block_size / probe_time` 比率动态计算最优预取深度，但在 CPU-bound 下区别不大 |

---

## 5. 内存分配优化

### 5.1 Cluster Block 内存分配改造（暂不实施）

> **当前只考虑单次查询，cluster block 的 buffer 不需要跨查询复用。** 后续实现批次查询时再添加 slab/pool 机制。

**涉及文件**：`src/query/overlap_scheduler.cpp` SubmitClusterRead()

```
当前 (overlap_scheduler.cpp:96):
  uint8_t* buf = new uint8_t[size32];  // 每次 malloc，probe 后 delete[]
```

**当前唯一需要做的改动**：将 `new uint8_t[]` 改为 `aligned_alloc(4096, ...)` 以支持 O_DIRECT：

```
uint8_t* buf = static_cast<uint8_t*>(
    aligned_alloc(4096, (size32 + 4095) & ~4095u));
```

| 维度 | 分析 |
|------|------|
| **收益** | O_DIRECT 的前提；对齐 buffer 对 DMA 更友好 |
| **权衡** | aligned_alloc 失败返回 nullptr（非异常），需检查；释放改用 `free()` 而非 `delete[]` |

**未来批次查询时的可选方案**（记录备用）：
- **方案 A：Slab Allocator**（按 size class 预分配 freelist，消除 malloc/free 系统调用）
- **方案 B：复用 BufferPool**（最简，~5 行改动，cluster block 也走 pool）
- **方案 C：Arena Allocator**（预分配大块内存，bump pointer 分配，查询结束后 reset）

### 5.2 BufferPool 对齐改造

**涉及文件**：`include/vdb/query/buffer_pool.h`、`src/query/buffer_pool.cpp`

```
当前:
  使用 std::unique_ptr<uint8_t[]> (new uint8_t[capacity])
  → 不保证对齐

优化:
  使用 aligned_alloc(4096, round_up(capacity, 4096))
  → 4KB 对齐, 满足 O_DIRECT 要求
```

| 维度 | 分析 |
|------|------|
| **代码改动** | ~15 行（改分配/释放 + custom deleter） |
| **收益** | O_DIRECT 前提；对齐 buffer 对 DMA 和 cache 更友好 |
| **风险** | aligned_alloc 失败返回 nullptr（非异常），需检查 |

### 5.3 pending_ HashMap 优化

**涉及文件**：`include/vdb/query/overlap_scheduler.h`

```
当前:
  std::unordered_map<uint8_t*, PendingIO> pending_;
  每次 CQE 做 find + erase，总 ~794 次/query

替代方案:
  将 PendingIO index 存入 io_uring user_data (64bit)
  用 std::vector<PendingIO> + slot allocator 替代 hashmap
```

| 维度 | 分析 |
|------|------|
| **代码改动** | ~30 行 |
| **收益** | 消除 hash 查找（794 × 200ns ≈ 160μs，占 0.13%） |
| **权衡** | 收益较小。如果做 registered buffer，已需 index-based 管理，可顺带解决 |

### 5.4 ready_clusters_ HashMap 优化

**涉及文件**：`include/vdb/query/overlap_scheduler.h`

```
当前:
  std::unordered_map<uint32_t, ParsedCluster> ready_clusters_;

替代:
  std::vector<std::optional<ParsedCluster>> 按 sorted index 索引
```

| 维度 | 分析 |
|------|------|
| **收益** | O(1) 直接索引 vs hash 查找 |
| **权衡** | map 最多 16 个 entry（prefetch_depth），收益不大。但代码更清晰 |

---

## 6. IO 路径架构优化

### 6.1 分阶段读 Cluster Block（暂不实施，记录备用）

```
当前: 一次读整个 block (Region1 + Region2 + Region3)

优化: 两阶段读取
  Stage 1: 只读 FastScan(Region1) + Address(Region3) (~88KB for dim=512)
    → S1 分类, 确定哪些向量需要 S2
  
  Stage 2: 按需读 ExRaBitQ entries (仅 S1 Uncertain 部分)
    → 只读需要 S2 的 entries, 大部分 ExRaBitQ 数据不读

IO 节省: 419KB → ~88KB + 少量 S2 entries ≈ -77% IO 量
```

**暂不做的原因**：当前单簇 block 太大时增加了流水线深度和延迟，在单 CPU 线程上效果可能不好。100M 规模（block=2.7MB）时收益更明显，届时可重新评估。

### 6.2 Cluster Block 预热缓存（暂不实施）

> **当前只考虑单次查询场景，暂不需要跨查询的 cluster block 缓存。** 后续实现批次查询时再添加。

```
设计思路（备用）:
  在第一次查询前，预先读取全部/高频 cluster blocks 到内存
  → 后续查询直接从缓存取，无 IO
```

| 维度 | 分析 |
|------|------|
| **收益** | 小数据集（.clu=105MB）全部缓存后查询零 IO |
| **权衡** | 内存消耗（100K→105MB, 1M→1GB, 10M→11GB）；与 page cache 功能重复；O_DIRECT 下需自己实现缓存 |

### 6.3 FetchMissingPayloads IO 合并

```
当前: 对每个 top-K 结果单独 PrepRead payload (top_k=10 → 10 次小读)

优化: 如果多个结果在 .dat 中物理相邻，合并成一次大读
```

| 维度 | 分析 |
|------|------|
| **收益** | 减少 SQE 数量 |
| **权衡** | top-K 结果通常分散在不同 cluster，物理相邻概率低；top_k 很小（10-20），优化空间有限 |

---

## 7. IO 粒度与硬件适配

### 7.1 IO 块大小与硬件限制

> **结论：不需要用户态拆分机制。** 内核会自动将超过 `max_hw_sectors_kb` 的请求拆分成多个 bio，且在当前数据规模下不会触发 io_uring async worker 慢路径。

```
需要在目标 NVMe 上检查:
  cat /sys/block/nvme0n1/queue/max_hw_sectors_kb    → 通常 128KB (IOMMU)
  cat /sys/block/nvme0n1/queue/max_segments          → 通常 127-255
  cat /sys/block/nvme0n1/queue/nr_requests           → 通常 1023
```

**内核处理大 IO 请求的机制**：

```
┌──────────────────────────────────────────────────────────────┐
│  用户态: io_uring_prep_read(sqe, fd, buf, 2MB, offset)      │
│                          │                                   │
│  内核 bio 层:            ▼                                   │
│    if (request_size > max_hw_sectors_kb):                    │
│        自动拆分成多个 bio segment                             │
│        每个 segment ≤ max_hw_sectors_kb                      │
│        bio chain → NVMe driver → 多个 NVMe commands          │
│                                                              │
│    if (bio数 ≤ max_segments):  ← 几乎总是满足                │
│        ✓ 走 fast path (io_uring inline completion)          │
│        ✓ 单个 CQE 返回, 用户态完全感知不到拆分               │
│                                                              │
│    if (bio数 > max_segments):  ← 需要极大单次请求才触发      │
│        ✗ 可能触发 io_uring async worker (io-wq)             │
│        ✗ 触发阈值: max_segments × max_hw_sectors_kb          │
│        ✗ 典型: 127 × 128KB = 15.9MB                         │
└──────────────────────────────────────────────────────────────┘
```

**当前各类 IO 请求 vs 硬件限制**：

| IO 类型 | 大小 | bio 数 (128KB/bio) | 是否需要拆分 |
|---------|------|---------------------|-------------|
| .dat VEC_ONLY | 4KB (padded) | 1 | 不需要 |
| .dat VEC_ALL | 4-8KB | 1 | 不需要 |
| .dat PAYLOAD | < 4KB | 1 | 不需要 |
| .clu block (100K, nlist=256) | avg 419KB | ~4 | 不需要 |
| .clu block (100M, nlist=40K) | avg 2.7MB | ~22 | 不需要 (22 << 127) |

**触发慢路径的临界点**：单次请求 > 15.9MB（127 × 128KB）。当前所有场景的最大单次读取为 2.7MB，远低于此阈值。

> **⚠ 注意事项：大 payload 场景的 IO 拆分**
>
> 当前代码（`IoUringReader::PrepRead` → `io_uring_prep_read`）对 IO 大小没有任何用户态限制或拆分，完全依赖内核 bio 层的透明拆分。这在 record < 15.9MB 时没有问题，但需要注意：
>
> - **如果未来 payload 存储原始媒体数据（RAW 图片、视频帧等），单条 record 可能超过 15.9MB**
> - 超过后内核可能触发 io_uring async worker（额外 context switch ~5-10μs），不是失败但会延迟退化
> - 届时需要在 `IoUringReader::PrepRead` 内部实现透明拆分：将大 IO 拆成多个 ≤ 4MB 的 SQE，内部聚合所有子 CQE 后再报告一个 `IoCompletion`，对 `OverlapScheduler` 完全透明
>
> **当前不需要实施**——预期 payload 不会超过此阈值。记录此项作为未来防御性设计的参考。

### 7.2 IO 大小与 NVMe flash page 匹配

> **结论：已满足。** `page_size=4096` 的 record padding 使得每次 .dat 读取都是 4KB 的整数倍，完美匹配 NVMe flash page。

```
当前 .dat 实际读取大小 (page_size=4096):
  VEC_ONLY: 读 4096B (vec=2048B + pad, 已 round up 到 page_size)
  VEC_ALL:  读 4096B 或 8192B (整数 page)

  → 100% NVMe flash page 利用率 ✓
  → 无跨页读取 ✓
```

---

## 8. 全局总览表

| # | 优化项 | 改动量 | 收益(单查询) | 适用规模 | 依赖关系 |
|---|--------|--------|-------------|---------|---------|
| 2.1 | DEFER_TASKRUN + SINGLE_ISSUER | 3 行 | p99 -2ms | 所有 | 内核≥5.19 |
| 2.2 | IOPOLL | 1 行 | ~160μs | 所有 | 依赖 3.1 (O_DIRECT) |
| 2.3 | SQPOLL | ~10 行 | ~0.8ms | 高并发 | 需权限, 与2.2组合最佳 |
| 2.4 | 注册缓冲区 (fixed buffers) | ~80 行 | ~50μs | 所有 | 需改 BufferPool + AsyncReader |
| 2.5 | 注册文件描述符 (fixed files) | ~15 行 | ~80μs | 所有 | 无 |
| 3.1 | O_DIRECT | ~100 行 | 防劣化 | >RAM 数据集 | 依赖 3.2 |
| 3.2 | .clu 文件布局 4KB 对齐 | ~50 行 | O_DIRECT前提 | - | Writer+Reader+版本 |
| 3.3 | .dat O_DIRECT | ~5 行 | 防劣化 | >RAM 数据集 | **已天然 4KB 对齐，仅需 buffer 对齐** |
| 3.4 | Lookup table bulk pread | ~20 行 | ~4ms(启动) | 所有 | 无 |
| 4.1 | ProbeCluster 分批 Submit | ~15 行 | ~0μs* | Uncertain多时 | *当前per-cluster读<1个 |
| 4.2 | WaitAndPoll 批量等待 | ~10 行 | ~10μs | 所有 | 无 |
| 4.3 | Sliding window 参数调优 | 0 行 | 视场景 | 多线程/大数据 | benchmark 驱动 |
| 5.1 | Cluster block aligned_alloc | ~10 行 | O_DIRECT前提 | 所有 | **暂只做对齐，批次复用后续添加** |
| 5.2 | BufferPool 对齐 | ~15 行 | O_DIRECT前提 | - | 依赖 3.1 决策 |
| 5.3 | pending_ hashmap 优化 | ~30 行 | ~160μs | 所有 | 无 |
| 5.4 | ready_clusters_ hashmap 优化 | ~15 行 | 微小 | 所有 | 无 |
| 6.1 | 分阶段读 cluster block | ~200 行 | -77% IO量 | 大数据集 | **暂不做** |
| 6.2 | Cluster block 预热缓存 | ~100 行 | 零IO查询 | 小数据集 | **暂不做，批次查询时添加** |
| 6.3 | FetchMissingPayloads IO 合并 | ~30 行 | ~几μs | 所有 | 无 |
| 7.1 | 控制 IO 块大小 | 0 行 | - | - | **不需要：内核自动拆分，不触发慢路径** |
| 7.2 | IO 大小对齐 NVMe flash page | 0 行 | - | - | **已满足：page_size=4096 使读取天然对齐** |

---

## 9. 依赖关系图

```
                    ┌──────────────┐
                    │ 2.1 DEFER_TR │ ← 独立, 立即可做
                    └──────────────┘

                    ┌──────────────┐
                    │ 3.4 bulk     │ ← 独立, 立即可做 (仅影响启动)
                    │ pread lookup │
                    └──────────────┘

  ┌──────────┐     ┌──────────────┐     ┌──────────────┐
  │3.2 .clu  │────▶│ 3.1 O_DIRECT │────▶│ 2.2 IOPOLL   │
  │  对齐    │     │ (.clu + .dat)│     └──────────────┘
  └──────────┘     └──────┬───────┘
                          │             ┌──────────────┐
  ┌──────────┐            ├────────────▶│ 2.3 SQPOLL   │
  │5.2 Buf   │────────────┘             └──────────────┘
  │  对齐    │
  └──────────┘     注: .dat 已天然 4KB 对齐 (page_size=4096)
  ┌──────────┐         只需 buffer 对齐即可开 O_DIRECT
  │5.1 align │─────┘
  │ _alloc   │
  └──────────┘

  ┌──────────┐     ┌──────────────────┐
  │5.1 (后续)│────▶│ 2.4 Registered   │  ← 批次查询时再做
  │Slab/Arena│     │ Buffers          │
  └──────────┘     └──────────────────┘

  ┌──────────┐
  │2.5 Fixed │ ← 独立, 低成本
  │ Files    │
  └──────────┘

  ┌──────────┐
  │4.1 分批  │ ← 独立, 但当前收益接近0
  │ Submit   │
  └──────────┘

  ┌──────────┐
  │4.2 批量  │ ← 独立
  │ Wait     │
  └──────────┘
```

---

## 10. 推荐实施路径

根据依赖关系和收益，分三条并行路径：

### 路径 A：io_uring flags（无依赖，立即可做）

```
2.1 DEFER_TASKRUN ──→ benchmark ──→ 评估是否继续
                                      │
                                      ▼
                                2.5 Fixed Files (可选)
```

### 路径 B：O_DIRECT 全链路

```
3.2 .clu 布局对齐 ──┐
                     ├──→ 3.1 O_DIRECT (.clu + .dat) ──→ 2.2 IOPOLL
5.2 Buffer 对齐 ────┘
5.1 aligned_alloc ──┘

注: .dat 已天然 4KB 对齐, 无额外改动
    只需 .clu 对齐 + 所有 buffer 用 aligned_alloc
```

### 路径 C：内存/提交优化（低优先级）

```
3.4 bulk pread (独立)
2.4 Registered Buffers ← 批次查询时再做
```

### NVMe 目标机器上首先要做的事

1. 检查内核版本（`uname -r`），确认 ≥ 5.19 以支持 DEFER_TASKRUN
2. 查看 NVMe 队列参数：
   ```bash
   cat /sys/block/nvme0n1/queue/max_hw_sectors_kb
   cat /sys/block/nvme0n1/queue/max_segments
   cat /sys/block/nvme0n1/queue/nr_requests
   cat /sys/block/nvme0n1/queue/logical_block_size
   ```
3. 用当前代码跑 benchmark，获取**冷读**（echo 3 > /proc/sys/vm/drop_caches）下的真实 io_wait 数据
4. 对比 page cache 热读 vs 冷读的性能差异，确定 O_DIRECT 的优先级
