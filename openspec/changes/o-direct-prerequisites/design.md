## Context

当前 .clu 文件布局中，cluster block 紧密排列，offset 不满足 4KB 对齐（benchmark 验证 256/256 均不对齐）。buffer 使用 `new uint8_t[]` 分配，不保证对齐。Lookup table 通过 ~2000 次逐字段 PreadValue 读取。这三项在 O_DIRECT 下均会失败或性能退化。

服务器硬件: NVMe Kioxia KCD61LUL3T84, logical_block_size=512, max_hw_sectors_kb=128。

## Goals / Non-Goals

**Goals:**
- .clu v8: 所有 cluster block 的 offset 和 read length 满足 4KB 对齐
- 所有 IO buffer 满足 4KB 对齐
- Lookup table 读取从 ~2000 次 pread 减少到 1 次
- 注册 fd 减少每个 SQE 的 fget/fput 原子操作开销

**Non-Goals:**
- 不在本次开启 O_DIRECT（留给 PHASE5-005）
- 不改变 .dat 文件布局（已天然 4KB 对齐）
- 不改变 AsyncReader 接口签名

## Decisions

### Decision 1: .clu v8 对齐布局

```
v7 (当前):
  [Header 285B][Lookup nlist×entry_sz][Block0][Block1]...
  offset 不对齐

v8 (新):
  [Header 285B][Pad→4KB][Lookup nlist×entry_sz][Pad→4KB]
  [Block0][Pad→4KB][Block1][Pad→4KB]...
  所有 block offset = 4KB 对齐
  所有 block read length = round_up(actual_size, 4KB)
```

Writer 改动:
- Open(): 写完 lookup table 后 pad 到 4KB 边界再开始写 block
- EndCluster(): 写完 mini-trailer 后 pad current_offset_ 到 4KB 边界
- Pad 字节为 0x00
- kFileVersion 从 7 改为 8

Reader 改动:
- Open(): 接受 version 7 或 8；version 8 时在读完 lookup table 后跳过 pad 到 4KB 边界
- GetBlockLocation(): block_offset 已经是对齐的（Writer 保证），read length 需 round_up 到 4KB

空间开销: 每个 block 最多额外 4095B，nlist=1024 时最多 ~4MB (<5% of .clu total)。

### Decision 2: Buffer 对齐

两处改动:

1. `overlap_scheduler.cpp` SubmitClusterRead(): `new uint8_t[size]` → `aligned_alloc(4096, round_up(size, 4096))`。释放从 `delete[]` 改为 `free()`。由于 `unique_ptr<uint8_t[]>` 默认调用 `delete[]`，需要自定义 deleter。

2. `buffer_pool.h/cpp`: 内部分配改为 `aligned_alloc(4096, round_up(capacity, 4096))`，释放改为 `free()`。

### Decision 3: Lookup table bulk pread

Reader::Open() 中:
- 计算 lookup table 总大小: `nlist × entry_size`
- 一次 pread 读取到临时 buffer
- 在内存中逐字段 memcpy/decode 填充 `info_.lookup_table`

这消除 ~2000 次 pread 系统调用，节省 ~4ms 启动时间。O_DIRECT 下逐字段小 pread 无法满足对齐，此优化变为必须。

### Decision 4: Fixed Files

IoUringReader 新增:
- `RegisterFiles(int clu_fd, int dat_fd)`: 调用 `io_uring_register_files()`
- `PrepRead` 新增 `bool fixed_file` 参数（默认 false，向后兼容）
- 内部: fixed_file=true 时设置 `sqe->flags |= IOSQE_FIXED_FILE`

OverlapScheduler 在初始化后调用 RegisterFiles。

## Risks / Trade-offs

- **[Risk] .clu v8 不兼容 v7** → Reader 检查版本号，v7 仍可读（不对齐但功能正确）。v8 由 Writer 保证对齐。
- **[Risk] aligned_alloc 返回 nullptr** → 需检查并处理，不能像 new 一样抛异常。
- **[Risk] round_up read length 可能读到文件末尾之后** → 最后一个 block 的 round_up 可能超过文件大小。pread 在超出文件末尾时返回短读（不是错误），需确保 CQE result 检查不误判短读为失败。
- **[Trade-off] Fixed Files 限制 fd 生命周期** → 注册后 fd 不能关闭/重新打开。当前只有 2 个静态 fd，无问题。
