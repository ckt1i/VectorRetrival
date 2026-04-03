## Why

当前 io_uring 初始化仅使用 `IORING_SETUP_CQSIZE` flag，缺少 `DEFER_TASKRUN` 和 `SINGLE_ISSUER` 优化。内核在 CQE 就绪时会随机抢占用户态线程执行 task_work，导致 CPU 密集的 ProbeCluster 计算被打断、cache line 被污染，是 p99 尾延迟毛刺的主要来源之一（当前 p99-p50 = 3.9ms）。此外，在 NVMe 硬件上尚无冷读（无 page cache）benchmark 数据，无法判断 O_DIRECT 链路优化的优先级。

## What Changes

- io_uring 初始化添加 `IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER` flags，延迟 task_work 到用户态主动 wait 时执行
- 添加 fallback 逻辑：内核不支持时去掉新 flags 重试初始化
- 在 bench_e2e 中新增 `--cold` 模式，通过 posix_fadvise(FADV_DONTNEED) 驱逐 page cache 后运行冷读查询，无需 sudo
- 对比热读 vs 冷读性能差异，为后续 O_DIRECT 优化提供数据支撑

## Capabilities

### New Capabilities
- `defer-taskrun`: io_uring DEFER_TASKRUN + SINGLE_ISSUER flag 支持及内核版本 fallback
- `cold-benchmark`: bench_e2e --cold 模式，posix_fadvise 驱逐 page cache + 热/冷对比

### Modified Capabilities

## Impact

- **代码**: `src/query/io_uring_reader.cpp` Init() 方法（~3 行 flag 改动 + ~10 行 fallback）
- **Benchmark**: `benchmarks/bench_e2e.cpp` 新增 `--cold` 参数和 posix_fadvise 驱逐逻辑
- **依赖**: 无新依赖；要求内核 >= 5.19（fallback 兼容旧内核）；posix_fadvise 为 glibc 内置，无需 sudo
- **兼容性**: SINGLE_ISSUER 限制该 ring 只能从创建线程操作，与当前单线程模型完全兼容；未来多线程查询需每线程一个 ring
