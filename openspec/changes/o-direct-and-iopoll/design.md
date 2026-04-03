## Context

O_DIRECT 前置已完成: .clu v8 4KB 对齐, buffer aligned_alloc, bulk pread。当前 open() 使用 O_RDONLY，IO 经过 page cache。100K+CRC 场景下 IO 仅占 3%，但大数据集 (>> RAM) 下 page cache thrashing 会成为问题。

服务器: 内核 6.8.0, NVMe Kioxia KCD61LUL3T84, max_hw_sectors_kb=128, max_segments=33。

## Goals / Non-Goals

**Goals:**
- 支持 O_DIRECT 绕过 page cache，通过参数控制开关
- 支持 IOPOLL 硬件轮询完成，通过参数控制
- 消除 pending_ hashmap 的 hash 查找开销
- 默认关闭 O_DIRECT 和 IOPOLL，保持现有行为

**Non-Goals:**
- 不自动决定何时开启 O_DIRECT（由调用方决定）
- 不实现 SQPOLL（需要额外 CPU 核心和可能的 root 权限）

## Decisions

### Decision 1: O_DIRECT 开关传递路径

```
bench_e2e --direct-io
  → Segment::Open(dir, schemas, use_direct_io=true)
    → ClusterStoreReader::Open(path, use_direct_io)
       fd_ = ::open(path, O_RDONLY | O_DIRECT)
    → DataFileReader::Open(path, dim, schemas, use_direct_io)
       fd_ = ::open(path, O_RDONLY | O_DIRECT)
```

ClusterStoreReader 和 DataFileReader 的 Open() 新增 `bool use_direct_io = false` 参数。Segment::Open() 透传。

注意: bulk pread 读 lookup table 时仍在 O_DIRECT fd 上执行。这要求 bulk pread 的 buffer 也是对齐的，且 read offset 和 length 满足对齐。header 区域的小读取在 Open() 中通过 bulk pread 已经解决（单次大读取满足对齐）。

### Decision 2: IOPOLL 开关

IoUringReader::Init() 新增 `bool use_iopoll = false`。启用时添加 `IORING_SETUP_IOPOLL`。

IOPOLL 将 CQE 完成方式从中断改为轮询。`io_uring_wait_cqe` 变为 busy-wait。在 CPU-bound 场景下可能浪费 CPU 周期。建议仅在 IO-bound（冷数据/大数据集）时开启。

IOPOLL 要求所有 IO 必须是 O_DIRECT（内核强制）。代码中不做强制检查，由调用方保证。

### Decision 3: pending_ hashmap → slot vector

当前: `unordered_map<uint8_t*, PendingIO> pending_`，每次 CQE 做 `find + erase`。

改为:
- `std::vector<PendingIO> slots_`（固定大小，与 io_queue_depth 对齐）
- `std::vector<uint32_t> free_slots_`（空闲 slot 栈）
- 分配 slot: `uint32_t idx = free_slots_.back(); free_slots_.pop_back();`
- PrepRead 时: `io_uring_sqe_set_data64(sqe, idx)` 将 slot index 编码到 user_data
- CQE 完成时: `uint32_t idx = cqe->user_data; auto& io = slots_[idx]; free_slots_.push_back(idx);`

这要求 IoCompletion 结构改为返回 user_data (uint64_t) 而不是 buffer 指针，或者 slot 中同时存储 buffer 指针。

**选择**: slot 中存储 buffer 指针。IoCompletion 不变（仍返回 buffer），但 OverlapScheduler 内部用 slot index 查找 PendingIO 而非 hash 查找。

实现方式: OverlapScheduler 分配 slot → 将 buffer + slot_index 一起传给 PrepRead → io_uring_sqe_set_data 编码为 `(slot_index << 32) | (buffer_ptr 的低 32 位)`... 这太复杂了。

**简化方案**: 直接在 OverlapScheduler 中将 `unordered_map<uint8_t*, PendingIO>` 改为 `std::vector<PendingIO> slots_` + `unordered_map<uint8_t*, uint32_t> buf_to_slot_`。CQE 返回 buffer → buf_to_slot_ 查 slot index → slots_[idx]。但这仍然是 hashmap...

**最终方案**: 改用 `io_uring_sqe_set_data64(sqe, slot_index)`。IoCompletion 返回 slot_index 而非 buffer。OverlapScheduler 从 slot 中取 buffer。这需要修改 AsyncReader 接口（IoCompletion 结构变化），影响面较大。

**决定: 推迟到独立 PR**。当前收益仅 ~160μs (0.13%)，修改 AsyncReader 接口影响面过大。记录在 UNDO.txt 即可。

## Risks / Trade-offs

- **[Risk] O_DIRECT 下 header 读取对齐**: Open() 中读 header 的小 pread 可能不满足 O_DIRECT 对齐要求 → 解决: header 读取在 bulk pread 之前完成，用对齐 buffer 读取 header 区域。
- **[Risk] IOPOLL busy-wait 浪费 CPU**: 当前 CPU-bound 场景下 IOPOLL 可能负收益 → 默认关闭，benchmark 验证后再决定。
- **[Risk] O_DIRECT 短读**: pread 在文件末尾附近可能返回短读 → 已有的 CQE result 检查需要容忍短读（result < requested 但 > 0 时不应视为错误）。
- **[Trade-off] pending_ hashmap 暂不替换**: 收益太小 (~160μs)，接口改动太大。
