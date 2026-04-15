## 1. 确认运行时 epsilon 语义

- [x] 1.1 在构建器和元数据加载路径中，记录 `epsilon-percentile` 到 `segment.meta.conann.epsilon` 的实际校准链路。
- [x] 1.2 验证并写明：当 `bench_e2e` 使用 `--index-dir` 复用索引时，运行时 `eps_ip` 直接来自已加载的索引元数据，而不会被查询阶段的 `--epsilon-percentile` 重新刷新。
- [x] 1.3 定义这次实验的事实来源字段和日志格式，包括加载后的运行时 `eps_ip`，以及每次运行到底是重建索引还是复用索引。

## 2. 准备固定点验证矩阵

- [x] 2.1 将主 warm-serving 服务点固定为 `coco_100k`、`queries=1000`、`topk=10`、`nlist=2048`、`nprobe=200`、`crc-alpha=0.02` 和 `clu_mode=full_preload`。
- [x] 2.2 定义第一轮重建网格为 `epsilon-percentile={0.90, 0.95, 0.99}`，并保持 `epsilon-samples` 不变。
- [x] 2.3 为每个重建索引分配独立输出目录和 tracker 记录行，避免不同 epsilon 点意外复用同一个运行时元数据。
- [x] 2.4 按照 `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_eps{epsilon-percentile}` 的规则存储重建后的索引文件，确保不同 epsilon 的索引不会互相覆盖。

## 3. 执行重建与复测

- [x] 3.1 针对每个 `epsilon-percentile` 点分别重建索引，并在 benchmark 前确认该索引加载到的运行时 `eps_ip`。
- [x] 3.2 使用完全相同的 warm-serving 参数对每个重建索引跑 benchmark，不改变数据集、查询数、`nprobe`、`crc-alpha` 或 `.clu` 模式。
- [x] 3.3 导出并收集每次运行所需的候选流和延迟指标，包括 `avg_total_probed`、Stage 1 `SafeOut`、Stage 2 `SafeOut`、Stage 2 `Uncertain`、`avg_probe_time_ms`、`avg_rerank_cpu_ms`、recall 和 p99。

## 4. 分析结果并决定下一条优化路线

- [x] 4.1 构建对比表，将构建时 `epsilon-percentile`、加载后的运行时 `eps_ip`、候选流统计、recall 和延迟在同一个固定点下对齐。
- [x] 4.2 按决策门槛判断下一步：如果 Stage 2 压力明显下降且召回没有明显回退，则继续 epsilon 调优；否则停止 epsilon 调优，转向 CPU 路径优化。
- [x] 4.3 将结论和下一步动作写回 `baselines/results/analysis.md` 以及 `refine-logs/EXPERIMENT_TRACKER*.md`，让后续实验可以直接沿着已验证的结果推进。
