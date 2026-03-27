# CRC Overlap Integration Benchmark

## Requirements

### R1: SafeOut 误杀率对比
对比两种 SafeOut 策略下的误杀率（false pruning rate）：
- **旧方法**: 静态 d_k 作为 SafeOut 阈值 → `Classify(dist, margin)`
- **新方法**: 动态 d_p_k 作为 SafeOut 阈值 → `ClassifyAdaptive(dist, margin, dynamic_d_k)`

误杀率定义：被 SafeOut 但实际上应该在 top-k 中的向量比例。需要用精确 L2 距离的 ground truth 计算。

输出指标：
- SafeOut count（旧 vs 新）
- SafeOut 误杀率（旧 vs 新）
- SafeIn count（旧 vs 新，预期不变）
- Uncertain count（旧 vs 新）

### R2: 端到端搜索时间
在 OverlapScheduler 中运行完整查询，测量：
- 总搜索时间 (total_time_ms)
- Probe 时间 (probe_time_ms)
- I/O 等待时间 (io_wait_time_ms)
- Rerank 时间 (rerank_time_ms)

对比配置：
- baseline: 旧 d_k 早停 + 静态 SafeOut + prefetch_depth=16
- CRC only: CRC 早停 + 静态 SafeOut + initial_prefetch=4
- CRC + dynamic: CRC 早停 + 动态 SafeOut + initial_prefetch=4

### R3: 流水线重叠比例
衡量 CPU probe 与 I/O 的重叠效率：
- `overlap_ratio = 1 - io_wait_time_ms / total_time_ms`
- 越接近 1 表示 I/O 完全被 CPU 工作掩盖

对比不同 initial_prefetch 值（4, 8, 16）下的 overlap_ratio。

### R4: 早停效率
- 平均 probed clusters（CRC vs d_k）
- 早停触发率
- clusters_skipped 分布

### R5: Recall 指标
- recall@1, recall@5, recall@10
- 对比旧/新配置确保 recall 不退化

## Acceptance Criteria

- [ ] Benchmark 能输出旧/新 SafeOut 误杀率对比表
- [ ] Benchmark 能输出端到端搜索时间对比表
- [ ] Benchmark 能输出 overlap_ratio 对比
- [ ] CRC + dynamic SafeOut 的搜索时间 ≤ 旧 d_k 方法
- [ ] Recall 不显著退化（recall@10 下降不超过 2%）
