## 1. O_DIRECT

- [x] 1.1 ClusterStoreReader::Open() 新增 use_direct_io 参数，启用时 open() 加 O_DIRECT
- [x] 1.2 ClusterStoreReader::Open() header 读取改用对齐 buffer（O_DIRECT 下小 pread 需要对齐）
- [x] 1.3 DataFileReader::Open() 新增 use_direct_io 参数，启用时 open() 加 O_DIRECT
- [x] 1.4 Segment::Open() + IvfIndex::Open() 透传 use_direct_io
- [x] 1.5 bench_e2e 新增 --direct-io 和 --iopoll flags

## 2. IOPOLL

- [x] 2.1 IoUringReader::Init() 新增 use_iopoll 参数，启用时加 IORING_SETUP_IOPOLL（含 fallback）
- [x] 2.2 bench_e2e --iopoll flag 传递到 IoUringReader::Init

## 3. 验证

- [x] 3.1 编译通过，bench_e2e --direct-io --bits 4 --queries 500 --cold 验证功能正确性
- [x] 3.2 bench_e2e --direct-io --iopoll 验证 IOPOLL 组合
