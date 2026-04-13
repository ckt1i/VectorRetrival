## Why

当前 BoundFetch 的单查询 warm-path 已经不是磁盘等待受限，而是 `io_uring` 提交路径的 CPU 开销受限。基于 COCO 100K、`nprobe=200`、500 queries 的最新 profiling，平均总延迟约 3.50ms，其中 `uring_submit_ms=1.86ms`（53.1%），而 `io_wait_ms=0.008ms`（0.2%）。这说明下一轮优化应聚焦 submit-side syscall、SQ 容量和提交流程组织，而不是继续把精力放在 payload 或设备侧等待上。

当前代码已经启用了 `DEFER_TASKRUN + SINGLE_ISSUER`，并接入了 fixed files；但 submit 点仍然分散、SQ depth 仍固定为 64、cluster read 与 vec read 共用单 ring 且互相挤占，导致批量提交收益被自动 flush 和频繁 refill submit 抵消。现在是把这些 submit-path 结构性问题系统收敛掉的合适时机。

## What Changes

- 新增针对 `io_uring` submit-path 的优化方案，覆盖统一 submit 节点、SQ depth 参数化、SQPOLL 评估和 ring 级隔离策略。
- 明确将 query 路径区分为两类 I/O：cluster block prefetch 与 vec/payload reads，并定义单 ring 优化与双 ring 拆分两种执行模式。
- 为 benchmark 增加 submit-path 相关控制项与观测项，用于验证 SQ depth、submit batching、SQPOLL、双 ring 的真实收益。
- 补充 completion dispatch 和 buffer 管理的微优化路线，包括 `pending_` 哈希表替换为 slot vector、fixed buffers 评估等。
- 在 proposal 与 design 中给出收益预估区间，作为实施后的验收参考。

## Capabilities

### New Capabilities
- `uring-submit-path`: 定义 query 路径中 `io_uring` submit-side 的组织方式、批量语义、ring 隔离策略和收益验收目标。

### Modified Capabilities
- `e2e-benchmark`: 新增 submit-path 相关 benchmark 控制参数与 timing 输出，用于评估 SQ depth、SQPOLL、单双 ring 配置的效果。

## Impact

- 影响搜索热路径：`src/query/io_uring_reader.cpp`、`src/query/overlap_scheduler.cpp`、`include/vdb/query/async_reader.h`、`include/vdb/query/search_context.h`
- 影响 benchmark 控制面：`benchmarks/bench_e2e.cpp`，可能扩展 `bench_vector_search.cpp`
- 可能引入新的运行模式与配置项：`io_queue_depth` 真正接入 ring 初始化、可选 `use_sqpoll`、可选双 ring / ring 配额隔离
- 可能引入新的内部数据结构：slot vector / freelist completion dispatch、registered buffer slabs
- 不计划改变搜索结果语义或召回逻辑；目标是降低 warm-path 延迟与 submit-side CPU 时间
- 预估效果：
  - 保守预期：`uring_submit_ms` 从 1.86ms 降到 0.9-1.2ms，整体平均延迟从 3.50ms 降到 2.5-2.8ms
  - 积极预期：若统一 submit + 更深 SQ + SQPOLL 叠加稳定，`uring_submit_ms` 可降到 0.5-0.8ms，整体平均延迟可到 2.0-2.4ms
  - 微优化项（slot vector / fixed buffers）预计额外贡献 0.1-0.2ms，而不是主收益来源
