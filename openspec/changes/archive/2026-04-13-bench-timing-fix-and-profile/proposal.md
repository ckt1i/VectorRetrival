## Why

当前 `bench_e2e` 的 timing 统计将 CPU 计算时间、io_uring 提交开销和调度等待时间混为一谈，导致无法准确判断 BoundFetch 在 WARM/COLD 状态下的真实 CPU vs I/O 瓶颈。同时缺少 nprobe 扫描实验，无法找到同等 recall@10 目标下的最优 nprobe 参数，也无法通过 perf 工具对关键阶段做更细粒度的硬件级 profiling。

## What Changes

- **修复 timing 统计**：将 `bench_e2e` 的 `avg_probe_time` 拆分为 FastScan 纯计算时间、ExRaBitQ reranking 时间、io_uring SQE 提交时间、io_uring CQE 等待时间，以及 overlap_scheduler 事件循环开销，分别计时并输出到 `results.json`
- **新增 nprobe 扫描模式**：`bench_e2e` 支持 `--nprobe-sweep <list>` 参数，对多个 nprobe 值依次运行，输出每个 nprobe 对应的 recall@10 与 avg_latency_ms，生成 sweep 结果 CSV
- **perf profiling 脚本**：新增 `benchmarks/scripts/run_perf_profile.sh`，封装 `perf stat` 和 `perf record` 调用，对 bench_e2e 的关键阶段（FastScan loop、ExRaBitQ、io_uring wait）进行 hardware counter 采集，配合 `perf report` / `flamegraph` 输出分析报告

## Capabilities

### New Capabilities

- `fine-grained-timing`: bench_e2e 输出细粒度分阶段 timing 统计（fastscan_ms、exrabitq_ms、uring_submit_ms、uring_wait_ms、scheduler_overhead_ms）
- `nprobe-sweep`: bench_e2e 支持多 nprobe 批量扫描，输出 recall-latency 曲线数据
- `perf-profiling`: shell 脚本封装 perf stat/record，对 bench_e2e 做 hardware-level profiling

### Modified Capabilities

<!-- no existing spec-level behavior changes -->

## Impact

- `benchmarks/bench_e2e.cpp`：新增 timing struct，修改 overlap_scheduler 调用点插入计时
- `vdb/query/overlap_scheduler.h/.cpp`：暴露分阶段计时接口（或通过回调注入）
- `benchmarks/scripts/run_perf_profile.sh`：新文件
- `baselines/results/`：sweep 结果写入 `nprobe_sweep_{dataset}.csv`
