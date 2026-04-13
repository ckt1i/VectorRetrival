## ADDED Requirements

### Requirement: SearchStats 包含 rerank_cpu_ms 字段
`SearchStats` 结构体 SHALL 新增 `double rerank_cpu_ms = 0` 字段，记录 `ConsumeVec` 和 `ConsumeAll` 调用的纯 CPU 时间（L2Sqr 计算 + TryInsert）。

#### Scenario: rerank_cpu_ms 默认值为零
- **WHEN** 构造 `SearchStats`
- **THEN** `rerank_cpu_ms` 初始化为 0.0

#### Scenario: ConsumeVec 调用时累加 rerank_cpu_ms
- **WHEN** `OverlapScheduler::DispatchCompletion` 处理 `VEC_ONLY` 类型完成事件并调用 `reranker.ConsumeVec()`
- **THEN** `ctx.stats().rerank_cpu_ms` 增加本次 `ConsumeVec` 的实际 CPU 时间（以毫秒为单位）

#### Scenario: ConsumeAll 调用时累加 rerank_cpu_ms
- **WHEN** `OverlapScheduler::DispatchCompletion` 处理 `VEC_ALL` 类型完成事件并调用 `reranker.ConsumeAll()`
- **THEN** `ctx.stats().rerank_cpu_ms` 增加本次 `ConsumeAll` 的实际 CPU 时间（以毫秒为单位）

### Requirement: bench_e2e 输出展示 rerank_cpu_ms
`bench_e2e` SHALL 在每个查询阶段（HOT/COLD）的统计输出行中展示 `avg_rerank_cpu_ms`，并在 JSON 输出文件中写入对应字段。

#### Scenario: 控制台输出包含 rerank_cpu_ms
- **WHEN** HOT 或 COLD 查询阶段完成
- **THEN** 日志输出行格式为：`io_wait=X.XXX ms  cpu=X.XXX ms  probe=X.XXX ms  rerank_cpu=X.XXX ms`

#### Scenario: JSON 输出包含 rerank_cpu_ms
- **WHEN** bench_e2e 写入结果 JSON 文件
- **THEN** JSON 对象中包含 `"avg_rerank_cpu_ms": <value>` 字段

#### Scenario: cpu 字段仍表示 total - io_wait
- **WHEN** 计算 `avg_cpu`
- **THEN** `avg_cpu = (total_time - io_wait_time) / Q`（保持向后兼容，`rerank_cpu_ms` 是新增字段，不替换 `cpu`）

### Requirement: rerank_cpu_ms 数值合理性
在正常搜索中，`rerank_cpu_ms` SHALL 满足：
- `rerank_cpu_ms` ≤ `total_time - io_wait_time`
- `probe_time_ms + rerank_cpu_ms` ≤ `total_time - io_wait_time`

#### Scenario: rerank_cpu 不超过 cpu 上界
- **WHEN** 对 COCO 100K 或 Deep1M 运行 1000 次查询后计算均值
- **THEN** `avg_rerank_cpu_ms` < `avg_cpu_ms`（即 `total - io_wait` 的均值）
