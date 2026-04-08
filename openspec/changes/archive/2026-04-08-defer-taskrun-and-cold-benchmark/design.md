## Context

当前 `IoUringReader::Init()` (io_uring_reader.cpp:25-28) 仅设置 `IORING_SETUP_CQSIZE` flag。io_uring CQE 就绪时内核立即执行 task_work，抢占正在执行 ProbeCluster CPU 密集计算的用户线程，导致 p99 尾延迟毛刺。

服务器环境: 内核 6.8.0-87-generic (>= 5.19, 支持 DEFER_TASKRUN)，NVMe Kioxia KCD61LUL3T84。

当前无冷读数据，benchmark 均在 page cache 热读下执行（io_wait=0.02ms），无法评估 O_DIRECT 优化的投入产出比。

## Goals / Non-Goals

**Goals:**
- 启用 DEFER_TASKRUN + SINGLE_ISSUER，消除内核随机抢占，稳定 p99 延迟
- 兼容旧内核: flag 不支持时自动 fallback 到当前行为
- 获取冷读 benchmark 数据，量化 IO 在无 page cache 时的真实占比
- 为后续 O_DIRECT 优化提供数据决策依据

**Non-Goals:**
- 不实施 O_DIRECT、IOPOLL、SQPOLL 等后续优化（由 benchmark 数据驱动决策）
- 不改变 AsyncReader 接口
- 不引入多线程支持

## Decisions

### Decision 1: DEFER_TASKRUN + SINGLE_ISSUER 组合

在 `io_uring_queue_init_params` 中添加两个 flag:
```
params.flags = IORING_SETUP_CQSIZE
             | IORING_SETUP_SINGLE_ISSUER
             | IORING_SETUP_DEFER_TASKRUN;
```

**理由**: DEFER_TASKRUN 要求 SINGLE_ISSUER (内核强制)。SINGLE_ISSUER 限制 ring 只能从创建线程操作，与当前单线程模型完全匹配。

**替代方案**: 仅 SINGLE_ISSUER 不加 DEFER_TASKRUN — 收益极小，SINGLE_ISSUER 本身仅做内部优化。

### Decision 2: Fallback 策略

Init() 中先尝试带新 flags 初始化。如果 `io_uring_queue_init_params` 返回错误（-EINVAL 表示内核不支持），去掉 DEFER_TASKRUN 和 SINGLE_ISSUER 重试。

**理由**: 零风险方案，保证在任何 >= 5.1 内核上都能正常运行。不需要编译时检测。

### Decision 3: WaitAndPoll 适配 DEFER_TASKRUN

启用 DEFER_TASKRUN 后，CQE 不会在后台自动产生。必须通过 `io_uring_wait_cqe` / `io_uring_submit_and_wait` / `io_uring_get_events` 显式触发内核处理 task_work。

当前 `WaitAndPoll` 已经调用 `io_uring_wait_cqe`，这会自动触发 task_work 处理。`Poll` (非阻塞 peek) 在 DEFER_TASKRUN 下也需要显式触发: 改用 `io_uring_get_events(&ring)` 后再 peek，确保已完成的 IO 能被看到。

### Decision 4: 冷读 Benchmark 设计

在 `bench_e2e` 中添加 `--cold` 参数，利用 `posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED)` 驱逐 .clu 和 .dat 的 page cache，无需 sudo 权限。

流程:
1. 正常跑一轮热读查询，记录 baseline 指标
2. 对 clu_fd 和 dat_fd 调用 `posix_fadvise(FADV_DONTNEED)` 驱逐 page cache
3. 立即跑第二轮冷读查询
4. 输出热读 vs 冷读的对比 (avg_query, io_wait, probe_time, overlap_ratio)

**理由**:
- `posix_fadvise(FADV_DONTNEED)` 不需要 root/sudo，Linux 内核在实践中几乎总是立即执行驱逐
- 只驱逐目标文件的 page cache，比 `drop_caches` 更精确
- 内置于 bench_e2e，无需额外 shell 脚本，消除手动操作误差
- bench_e2e 是唯一经过完整 io_uring → .clu/.dat 路径的 benchmark（bench_vector_search 是纯内存计算，不走磁盘 IO）

**替代方案**: `echo 3 > /proc/sys/vm/drop_caches`（需 sudo，清除全系统 cache，过于粗暴）。

## Risks / Trade-offs

- **[Risk] SINGLE_ISSUER 限制 ring 创建线程** → 当前单线程完全兼容。未来多线程需每线程一个 ring (UNDO.txt PHASE6-001 已记录)。
- **[Risk] DEFER_TASKRUN 下 Poll 语义变化** → 在 Poll() 中添加 `io_uring_get_events` 调用确保 CQE 可见。
- **[Risk] posix_fadvise 非强制性** → FADV_DONTNEED 是 advisory，内核可以忽略。实践中 Linux 几乎总是执行驱逐，但需验证（可通过 /proc/meminfo Cached 差值确认）。
- **[Trade-off] 增加 Init 代码路径 (fallback)** → 代价极低(~10行)，换来跨内核兼容性。
