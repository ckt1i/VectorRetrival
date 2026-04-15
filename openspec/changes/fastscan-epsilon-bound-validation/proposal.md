## 为什么要做这件事

当前对 `coco_100k` 的 R067 profiling 显示，在 `.clu` `full_preload` 之后，查询 CPU 时间主要消耗在 `probe` 上，而其中相当一部分候选会继续进入 Stage 2，最后又在 Stage 2 被 `SafeOut` 拒掉。也就是说，当前瓶颈未必是 Stage 2 本身太慢，也可能是 Stage 1 的 FastScan 边界过于保守，导致太多候选流进了 Stage 2。

因此，在继续规划 CPU kernel 优化之前，需要先确认两件事：

1. 现有的 FastScan `epsilon-percentile` 是否仍然会通过重建索引改变运行时的 `eps_ip`。
2. 这个 `eps_ip` 是否真的会影响 Stage 1 的边界宽度，并进一步影响 Stage 2 的压力。

## 要改什么

- 制定一套聚焦的实验方案，用来验证构建阶段的 `epsilon-percentile` 如何影响运行时 FastScan 的 Stage 1 `SafeOut` / `Uncertain` / Stage 2 流量。
- 明确校验当前的校准链路：
  - `epsilon-percentile` 是否仍然用于 FastScan `eps_ip`
  - 在复用 `--index-dir` 时，这个影响是否只发生在构建阶段
  - `segment.meta` 和 `.clu` 中哪些字段真正决定了运行时边界
- 在 `coco_100k` 上建立受控的“重建-复测”流程，只变化 `epsilon-percentile`，其余服务侧参数保持固定。
- 每个不同 `epsilon-percentile` 重建后的索引都要落到独立目录，统一写入 `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_eps{epsilon-percentile}` 这种路径，避免不同实验点覆盖彼此的索引文件。
- 要求 benchmark 和分析结果输出足够的 Stage 1 / Stage 2 统计量，以判断瓶颈到底是：
  - FastScan `eps_ip` 过于保守
  - 还是 Stage 2 / SIMD CPU kernel 确实需要优化
- 设定清晰的决策门槛：
  - 如果降低 `epsilon-percentile` 能明显减少 Stage 2 压力且不损失召回，则继续做 epsilon 调优
  - 如果召回下降明显，或者 Stage 2 压力没有实质变化，则停止 epsilon 调优，转向 CPU kernel 优化

## 能力边界

### 新增能力
- `fastscan-epsilon-validation`：定义实验和验证协议，用于确认 `epsilon-percentile` 如何标定 FastScan `eps_ip`、这个 `eps_ip` 如何塑造 Stage 1 边界，以及如何执行和判断基于重建的 epsilon 扫描。

### 修改能力
- `e2e-benchmark`：扩展 benchmark 报告要求，使 FastScan epsilon 实验能够比较 `eps_ip`、Stage 1 `SafeOut`、Stage 2 `SafeOut`、Stage 2 `Uncertain`、probe 延迟和召回。
- `fix-conann-epsilon`：明确当前运行时 FastScan 边界由构建阶段校准出的 `segment.meta.conann.epsilon` 决定，`epsilon-percentile` 只有在重建索引时才会影响运行时行为。

## 影响范围

- 受影响的实验流程：
  - `benchmarks/bench_e2e.cpp`
  - `--epsilon-percentile` 的 prebuilt index / rebuilt index 使用方式
  - `baselines/results/e2e_comparison_warm.csv`
  - `baselines/results/analysis.md`
- 受影响的构建 / 元数据路径：
  - `src/index/ivf_builder.cpp`
  - `src/index/ivf_index.cpp`
  - `schema/segment_meta.fbs`
- 受影响的查询路径解释：
  - `src/query/overlap_scheduler.cpp`
  - `src/index/cluster_prober.cpp`
  - `src/simd/fastscan.cpp`
- 受影响的跟踪文档：
  - `refine-logs/EXPERIMENT_PLAN*.md`
  - `refine-logs/EXPERIMENT_TRACKER*.md`
