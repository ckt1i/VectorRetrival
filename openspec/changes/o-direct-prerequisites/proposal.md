## Why

为 O_DIRECT 做前置准备。O_DIRECT 要求所有 IO 的 buffer、offset、length 满足 4KB 对齐，当前 .clu 文件布局和 buffer 分配均不满足。此外 Lookup table 的逐字段 ~2000 次小 pread 在 O_DIRECT 下会因对齐违反而直接失败。同时 Fixed Files (注册 fd) 是独立的低成本优化，可顺带实施。

## What Changes

- **PHASE5-003**: .clu 文件布局 4KB 对齐 — Writer 侧在每个 cluster block 结束后 pad 到 4KB 边界，lookup table 区域也 pad 到 4KB 边界。文件版本号从 v7 升至 v8。**BREAKING**: 旧 .clu 文件不兼容，需重建索引。
- **PHASE5-004**: Buffer 分配对齐 — cluster block 分配和 BufferPool 内部分配改为 `aligned_alloc(4096, ...)`，释放改用 `free()`。
- **PHASE5-007**: Lookup table bulk pread — Reader 侧用一次 pread 读取整个 lookup table，在内存中 parse，替代当前 ~2000 次逐字段 PreadValue。
- **PHASE5-002**: Fixed Files — io_uring 注册 clu_fd + dat_fd，PrepRead 使用 fd_index + IOSQE_FIXED_FILE。

## Capabilities

### New Capabilities
- `clu-alignment`: .clu 文件 4KB 对齐布局 (v8) 及 Reader 兼容
- `buffer-alignment`: IO buffer 4KB 对齐分配
- `bulk-lookup-read`: Lookup table 一次性 bulk pread
- `fixed-files`: io_uring 注册文件描述符

### Modified Capabilities

## Impact

- **代码**: `src/storage/cluster_store.cpp` (Writer + Reader ~100 行改动), `src/query/overlap_scheduler.cpp` (~10 行), `include/vdb/query/buffer_pool.h` + `src/query/buffer_pool.cpp` (~15 行), `src/query/io_uring_reader.cpp` + `include/vdb/query/async_reader.h` (~20 行)
- **兼容性**: .clu v7 文件不兼容 v8，需重建索引。Reader 应检查版本号并给出明确错误
- **依赖**: 无新外部依赖
- **Benchmark**: 所有使用 .clu 文件的 benchmark 需要用新版 Writer 重建索引后才能运行
