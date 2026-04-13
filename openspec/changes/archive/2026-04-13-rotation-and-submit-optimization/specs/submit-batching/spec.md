## ADDED Requirements

### Requirement: SearchConfig 新增 submit_batch_size 参数
`SearchConfig` SHALL 新增 `uint32_t submit_batch_size = 8` 字段，控制 ProbeAndDrainInterleaved 中每 N 个 cluster probe 后执行一次 Submit。值为 0 表示每个 cluster 后立即 Submit（当前行为）。

#### Scenario: 默认 batch_size=8
- **WHEN** 使用默认 SearchConfig
- **THEN** `submit_batch_size` 等于 8

#### Scenario: batch_size=0 保持原有行为
- **WHEN** 设置 `submit_batch_size = 0`
- **THEN** ProbeAndDrainInterleaved 在每个 cluster probe 后立即 Submit（与当前行为一致）

### Requirement: IoUringReader 暴露 prepped() 接口
`IoUringReader` SHALL 提供 `uint32_t prepped() const` 方法，返回已 PrepRead 但尚未 Submit 的 SQE 数量。

#### Scenario: PrepRead 后 prepped 递增
- **WHEN** 连续调用 3 次 `PrepRead()` 且未调用 `Submit()`
- **THEN** `prepped()` 返回 3

#### Scenario: Submit 后 prepped 归零
- **WHEN** 调用 `PrepRead()` 3 次后调用 `Submit()`
- **THEN** `prepped()` 返回 0

### Requirement: ProbeAndDrainInterleaved 批量 Submit
`ProbeAndDrainInterleaved` SHALL 将 per-cluster Submit 改为计数器模式：每 `submit_batch_size` 个 cluster probe 后执行一次 Submit。在循环结束或 early_stop 时 SHALL flush 残余 SQEs。

#### Scenario: batch=8 时 Submit 次数减少
- **WHEN** nprobe=200, submit_batch_size=8
- **THEN** Submit 调用次数 ≤ 200/8 + refill_submits（约 25-30 次，而非当前的 ~200 次）

#### Scenario: early_stop 时 flush 残余
- **WHEN** 在第 50 个 cluster 处触发 early_stop，submit_batch_size=8
- **THEN** 残余的未提交 SQEs（最多 batch_size-1=7 个）SHALL 在退出循环前被 Submit

### Requirement: WaitAndPoll 前 safety flush
在 `ProbeAndDrainInterleaved` 的 WaitAndPoll 调用前，如果 `reader_.prepped() > 0`，SHALL 先调用 `reader_.Submit()` 以防止死锁（等待的 IO 尚未提交到内核）。

#### Scenario: 避免 WaitAndPoll 死锁
- **WHEN** 有 3 个 SQE 已 PrepRead 但未 Submit，且需要 WaitAndPoll 等待 cluster block IO
- **THEN** WaitAndPoll 前先 Submit 这 3 个 SQE，然后正常等待完成事件

### Requirement: bench_e2e 新增 --submit-batch 参数
`bench_e2e` SHALL 支持 `--submit-batch N` 命令行参数，传递给 `SearchConfig::submit_batch_size`。默认值为 8。

#### Scenario: 命令行指定 batch size
- **WHEN** 运行 `bench_e2e --submit-batch 16 ...`
- **THEN** SearchConfig 的 submit_batch_size 设为 16

#### Scenario: 不指定时使用默认值
- **WHEN** 运行 `bench_e2e` 不带 `--submit-batch`
- **THEN** SearchConfig 的 submit_batch_size 使用默认值 8
