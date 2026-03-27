## Why

`crc-overlap-integration` 已实现 CRC 集成到 OverlapScheduler（两层剪枝 + 动态 SafeOut + CRC 早停），但缺乏端到端验证。当前只有 `bench_crc_overlap` 做纯算法层面的 SafeOut 率对比（内存 KMeans，无真实 I/O），无法衡量：

1. CRC 在真实磁盘 I/O 流水线下的性能收益
2. 动态 SafeOut 对搜索时间的实际影响
3. 流水线重叠比例（overlap_ratio）是否因 SafeOut 增多而改善
4. CRC 早停 vs d_k 早停在真实索引上的 recall 差异

需要一个端到端 benchmark，**从磁盘索引加载 ClusterData**（而非使用 build 后的内存数据），对比 baseline（d_k 早停）和 CRC 模式下的完整搜索性能。

## What Changes

在 `bench_e2e.cpp` 中新增 **CRC 对比模式**：

- **Phase C.5（CRC 标定）**: Build 索引后，重新从磁盘打开索引，通过 `IvfIndex` + `Segment` + `ParsedCluster` API 从 `.clu` 文件读取 ClusterData（codes_block、raw vectors），执行 `CrcCalibrator::CalibrateWithRaBitQ` 得到 CalibrationResults
- **Phase D 扩展（双轮查询）**: 第一轮 baseline（`crc_params=nullptr`），第二轮 CRC 模式（`crc_params` 设为标定结果）
- **Phase E 扩展（对比评估）**: 输出两轮的 recall、搜索时间、SafeOut 率、早停率、overlap_ratio 的对比表

## Capabilities

### New Capabilities

- `crc-e2e-comparison`: bench_e2e 中 baseline vs CRC 双轮对比
- `disk-cluster-data-loader`: 从磁盘索引加载 ClusterData 用于 CRC 标定

### Modified Capabilities

- `bench-e2e`: 新增 `--crc` 开关启用 CRC 对比模式

## Impact

- `benchmarks/bench_e2e.cpp` — 修改：新增 CRC 标定 phase + 双轮查询 + 对比输出
- `benchmarks/CMakeLists.txt` — 修改：bench_e2e 新增 `vdb_crc` 链接
- 现有 bench_e2e 默认行为不变（`--crc` 未传时无影响）
