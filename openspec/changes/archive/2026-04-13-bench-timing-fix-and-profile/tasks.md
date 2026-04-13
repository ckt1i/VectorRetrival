## 1. SearchStats 扩展：新增细粒度 timing 字段

- [x] 1.1 在 `include/vdb/query/search_context.h` 的 `SearchStats` 结构体中新增 4 个字段：`uring_prep_ms`、`uring_submit_ms`、`parse_cluster_ms`、`fetch_missing_ms`（double，默认 0）
- [x] 1.2 在 `src/query/overlap_scheduler.cpp` 的 `AsyncIOSink::OnCandidate()` 中，对 `PrepRead()` 调用前后插入 `steady_clock` 计时，累加到 `sched_.ctx_stats_.uring_prep_ms`（或通过 `ProbeCluster` 结束后写入 `ctx.stats().uring_prep_ms`）
- [x] 1.3 在 `ProbeAndDrainInterleaved()` 中，对每个 `reader_.Submit()` 调用前后计时，累加到 `ctx.stats().uring_submit_ms`
- [x] 1.4 在 `DispatchCompletion()` 的 `CLUSTER_BLOCK` 分支中，对 `ParseClusterBlock()` 调用前后计时，累加到 `ctx.stats().parse_cluster_ms`
- [x] 1.5 在 `Search()` 中，对 `FetchMissingPayloads()` 调用前后计时，累加到 `ctx.stats().fetch_missing_ms`

## 2. bench_e2e：输出新 timing 字段

- [x] 2.1 在 `benchmarks/bench_e2e.cpp` 的 `QueryResult` 结构体中新增对应字段：`uring_prep_ms`、`uring_submit_ms`、`parse_cluster_ms`、`fetch_missing_ms`
- [x] 2.2 在 `RunQueryRound()` 中从 `results.stats()` 读取这 4 个字段并存入 `QueryResult`
- [x] 2.3 在 `RoundMetrics` 中新增 `avg_uring_prep`、`avg_uring_submit`、`avg_parse_cluster`、`avg_fetch_missing` 字段，并在聚合计算中填充
- [x] 2.4 在 `results.json` 的 `pipeline_stats` 输出块中新增这 4 个字段（avg 值），并在 stdout 打印时增加对应行

## 3. bench_e2e：nprobe sweep 模式

- [x] 3.1 解析 `--nprobe-sweep <comma-list>` 参数，将其拆分为 `std::vector<int>` nprobe_list；若同时提供 `--nprobe` 则打印错误并 exit(1)
- [x] 3.2 当 `nprobe_list` 非空时，进入 sweep 模式：跳过单次 `--nprobe` 的运行路径，改为循环遍历 `nprobe_list`
- [x] 3.3 在每个 nprobe 迭代中：(a) 重新设置 `search_cfg.nprobe`；(b) 运行 warmup（100 queries）；(c) 运行 measurement（1000 queries）；(d) 计算并打印 `[sweep] nprobe=N  recall@10=X.XXXX  avg=X.XXXms  probe=X.XXXms  safe_out_rate=XX.X%`
- [x] 3.4 在 `{output_dir}/nprobe_sweep.csv` 中写入或追加结果行（若文件不存在则先写入 header：`nprobe,recall@1,recall@10,avg_ms,p50_ms,p99_ms,avg_probe_ms,avg_io_wait_ms,avg_safe_out_rate`）
- [x] 3.5 运行验证：执行 `--nprobe-sweep 50,100,150,200 --dataset coco_100k --index-dir <existing>` 确认输出 4 行 CSV，recall 和 latency 数据合理

## 4. perf profiling 脚本

- [x] 4.1 创建 `benchmarks/scripts/run_perf_profile.sh`，设置 shebang `#!/usr/bin/env bash`，解析 `$BENCH_BIN`、`$BENCH_ARGS`、`$OUT_DIR` 三个参数（支持位置参数和环境变量）
- [x] 4.2 实现参数校验：若 `BENCH_BIN` 为空或不存在则打印 usage 并 exit 1
- [x] 4.3 实现 `perf stat` 模式：检测 `perf` 是否在 PATH；若在则运行 `perf stat -e cycles,instructions,cache-misses,cache-references,branch-misses,LLC-load-misses,LLC-store-misses $BENCH_BIN $BENCH_ARGS`，将输出写入 `$OUT_DIR/perf_stat.txt`；若 perf 不存在则打印 warning 跳过
- [x] 4.4 实现 `perf record` 模式：检测 `/proc/sys/kernel/perf_event_paranoid` 值；若 ≤ 1 则运行 `perf record -g --call-graph dwarf -o $OUT_DIR/perf.data $BENCH_BIN $BENCH_ARGS` + `perf report --stdio > $OUT_DIR/perf_report.txt`；若 > 1 则打印 note 跳过
- [x] 4.5 实现 FlameGraph 可选路径：检测 `flamegraph.pl` 是否在 PATH 或 `../FlameGraph/flamegraph.pl`；若在且 `perf.data` 已生成，则运行 `perf script | stackcollapse-perf.pl | flamegraph.pl > $OUT_DIR/flamegraph.svg`
- [x] 4.6 打印 summary block：列出生成的文件列表、查看命令（`less perf_report.txt`、`open flamegraph.svg`），以及下一步分析建议

## 5. 编译验证

- [x] 5.1 在 `build-bench-only/` 目录下重新编译 bench_e2e：`cmake --build . --target bench_e2e -j$(nproc)` 确认无编译错误
- [x] 5.2 运行快速冒烟测试：`bench_e2e --index-dir <existing_coco> --nprobe 200 --queries 10` 确认新字段出现在 `results.json` 的 `pipeline_stats` 中
- [x] 5.3 运行 nprobe sweep 冒烟测试：`bench_e2e --index-dir <existing_coco> --nprobe-sweep 50,200 --queries 10` 确认 `nprobe_sweep.csv` 有 2 行数据
- [x] 5.4 运行 perf 脚本冒烟测试：`BENCH_BIN=./bench_e2e BENCH_ARGS="--index-dir <idx> --nprobe 100 --queries 50" bash benchmarks/scripts/run_perf_profile.sh` 确认 `perf_stat.txt` 生成（注：本机 perf_event_paranoid=4，perf stat 被内核拒绝，脚本已正确输出 warning 并跳过）
