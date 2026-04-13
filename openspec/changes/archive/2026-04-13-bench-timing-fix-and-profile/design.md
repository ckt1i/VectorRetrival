## Context

`bench_e2e` 当前使用 `SearchStats` 结构体记录 4 个时间字段：

| 字段 | 计时点 | 包含内容 |
|------|--------|---------|
| `total_time_ms` | 整个 `Search()` wall clock | 所有阶段 |
| `probe_time_ms` | `ProbeCluster()` 调用 | FastScan + ExRaBitQ Stage 2 + `PrepRead()` io_uring SQE 准备 |
| `io_wait_time_ms` | `WaitAndPoll()` 阻塞 | kernel 等待 CQE 返回 |
| `rerank_cpu_ms` | `ConsumeVec`/`ConsumeAll` | 从磁盘读回向量后的精确 L2 计算 |

**关键问题**：`total - io_wait - probe - rerank ≈ 1.18ms`（WARM 实测）是完全未计时的开销，包含：
- `ParseClusterBlock()` — 将 io_uring 读回的 cluster block 解析为 `ParsedCluster` 结构
- `reader_.Submit()` — io_uring SQE batch 提交（在每个 cluster probe 之后调用）
- `FetchMissingPayloads()` — 扫尾读取 SafeIn/Uncertain 候选的 payload
- 事件循环本身的开销（`ready_clusters_` 查找、`DispatchCompletion` dispatch）

另外 `probe_time_ms` 混入了 `AsyncIOSink::OnCandidate()` → `PrepRead()` 的 io_uring 队列写入开销，并非纯 CPU 计算时间。

## Goals / Non-Goals

**Goals:**
- 将 `SearchStats` 扩展为细粒度分阶段 timing，能区分：FastScan CPU、ExRaBitQ CPU、io_uring SQE 准备、io_uring Submit、ParseCluster、WaitAndPoll（CQE）、FetchMissing
- 新增 `bench_e2e --nprobe-sweep` 模式，对多个 nprobe 值批量运行，输出 recall-latency CSV
- 新增 `benchmarks/scripts/run_perf_profile.sh` 封装 `perf stat` + `perf record` 调用，针对 bench_e2e 关键阶段

**Non-Goals:**
- 不修改 `OverlapScheduler` 的核心逻辑或算法
- 不改变 `results.json` 的现有字段（只新增）
- 不引入多线程或异步 profiling 框架（保持单线程模型）
- 不实现 nprobe 自动调参（只做扫描，不做优化）

## Decisions

### D1：细粒度 timing 通过回调注入，不修改 ClusterProber 内部

**选项 A**: 直接在 `ClusterProber::Probe()` 内部插入计时，分别记录 FastScan 和 ExRaBitQ 的时间。  
**选项 B**: 在 `OverlapScheduler::ProbeCluster()` 中拆分：先计时 `PrepareQueryInto()`，再计时 `prober_.Probe()`，把 `PrepRead()` 时间从 probe 中拆出来。  
**选项 C**: 在 `ProbeCluster()` 外包裹计时，在 `AsyncIOSink::OnCandidate()` 内用单独计时器统计 `PrepRead()` 时间。

**选择 C**（最小修改量）：
- 在 `AsyncIOSink` 内部增加 `double prep_read_ms` 计数器，`OnCandidate()` 里对 `PrepRead()` 调用前后加 `steady_clock`
- `ProbeCluster()` 结束后把 `sink.prep_read_ms` 累加到 `ctx.stats().uring_prep_ms`
- `probe_time_ms` 保持不变（仍是 `ProbeCluster()` wall clock），新增 `uring_prep_ms` 可以从中减去得到纯 CPU 探针时间
- `ParseClusterBlock()` 在 `DispatchCompletion()` 的 CLUSTER_BLOCK 分支计时，累加到新字段 `parse_cluster_ms`
- `reader_.Submit()` 调用前后计时，累加到 `uring_submit_ms`
- `FetchMissingPayloads()` 调用前后计时，累加到 `fetch_missing_ms`

### D2：`SearchStats` 新增字段，`results.json` 新增对应输出

新增字段（全部初始化为 0）：

```cpp
double uring_prep_ms = 0;      // PrepRead() in AsyncIOSink::OnCandidate()
double uring_submit_ms = 0;    // reader_.Submit() calls
double parse_cluster_ms = 0;   // ParseClusterBlock() in DispatchCompletion
double fetch_missing_ms = 0;   // FetchMissingPayloads()
```

`bench_e2e` 的 `pipeline_stats` JSON block 新增这 4 个字段的 avg 值。

### D3：nprobe sweep 实现为 bench_e2e 的命令行模式

新增 `--nprobe-sweep <comma-list>` 参数（如 `--nprobe-sweep 50,100,150,200`）：
- 与 `--nprobe` 互斥（不能同时使用）
- 对每个 nprobe 值依次执行完整的 warmup（100 query）+ measurement（1000 query）
- 将每个 nprobe 的 `{nprobe, recall@10, avg_ms, p50_ms, p99_ms, avg_probe_ms}` 输出到 sweep CSV
- CSV 路径：`{output_dir}/nprobe_sweep.csv`，同时打印到 stdout

### D4：perf 脚本用 perf stat 采集 hardware counters，用 perf record + 火焰图采集 call stack

脚本 `benchmarks/scripts/run_perf_profile.sh` 接受参数：
- `BENCH_BIN`：bench_e2e 可执行文件路径  
- `BENCH_ARGS`：传给 bench_e2e 的参数（不含 perf 相关）
- `OUT_DIR`：输出目录（默认 `/tmp/perf_profile_$(date +%Y%m%dT%H%M%S)`）

模式：
1. `perf stat` — 采集 `cycles, instructions, cache-misses, cache-references, branch-misses, LLC-load-misses, LLC-store-misses`
2. `perf record -g --call-graph dwarf` + `perf report` — 调用栈热点（需要 debug info）
3. 可选：`FlameGraph` 脚本生成 SVG（检测是否有 `flamegraph.pl` 在 PATH）

## Risks / Trade-offs

- **`steady_clock` overhead**: 在 `AsyncIOSink::OnCandidate()` 内加 `steady_clock::now()` 两次，对高频路径（每个候选一次）有微小影响（~10-50ns per call）。WARM 下 nprobe=200，每 cluster 约 50 candidates，总调用约 10K 次/query → 额外开销约 0.1-0.5ms。可通过 `#ifdef DETAILED_TIMING` 条件编译关闭。  
  **缓解**：在 sweep 模式下启用，在 production bench 模式下用宏关闭。

- **perf 权限**：`perf record` 需要 `perf_event_paranoid <= 1` 或 `CAP_PERFMON`。若无权限，脚本降级到仅 `perf stat`（不需要特殊权限），并给出提示。

- **nprobe sweep 运行时间**：每个 nprobe 值跑 1100 query（100 warmup + 1000 measurement），4 个 nprobe 值 ≈ 4×5s = 20s。可接受。
