## Why

所有 O_DIRECT 前置条件已完成（.clu v8 4KB 对齐、buffer aligned_alloc、bulk pread）。现在可以开启 O_DIRECT，绕过 page cache，实现 DMA 直传。这是大数据集（数据 >> RAM）下避免 page cache thrashing 的关键。同时 IOPOLL（硬件轮询完成）依赖 O_DIRECT，可以消除 NVMe 中断开销。此外 pending_ hashmap 替换为 slot vector 是一个独立的小优化，消除每次 CQE dispatch 的 hash 查找。

## What Changes

- **PHASE5-005 O_DIRECT**: ClusterStoreReader::Open() 和 DataFileReader::Open() 支持 O_DIRECT flag，通过参数控制开关
- **PHASE5-006 IOPOLL**: IoUringReader::Init() 支持 IORING_SETUP_IOPOLL flag，通过参数控制，需 O_DIRECT 为前提
- **PHASE5-008 pending_ hashmap → slot vector**: 将 PendingIO index 编码到 io_uring user_data，用 vector + freelist 替代 unordered_map

## Capabilities

### New Capabilities
- `o-direct`: 通过参数控制 .clu 和 .dat 文件的 O_DIRECT 开关
- `iopoll`: 通过参数控制 IORING_SETUP_IOPOLL 硬件轮询完成
- `slot-pending`: 用 slot vector 替代 pending_ hashmap，消除 hash 查找开销

### Modified Capabilities

## Impact

- **代码**: `src/storage/cluster_store.cpp` (~5行), `src/storage/data_file_reader.cpp` (~5行), `src/storage/segment.cpp` + `segment.h` (~10行), `src/query/io_uring_reader.cpp` (~5行), `src/query/overlap_scheduler.cpp` + `.h` (~50行)
- **配置**: Segment::Open() 新增 `bool use_direct_io` 参数；IoUringReader::Init() 新增 `bool use_iopoll` 参数
- **兼容性**: 默认 O_DIRECT=off 保持向后兼容；开启需要 v8 .clu 文件
