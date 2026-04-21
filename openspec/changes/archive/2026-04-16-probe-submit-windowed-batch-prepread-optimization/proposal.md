## Why

当前 `probe_submit` 链路已经完成了 batch dedup 与 CRC 语义修复，但在线路径里 `probe_submit_ms` 和 `uring_submit_ms` 仍然没有明显下降。现有实现仍然以较碎的请求粒度组织 `PrepReadVecOnly/All` 和 `io_uring_submit`，导致 resident 模式下 query 主路径仍然为重复的用户态准备和过多的 submit 次数付出固定成本。

## What Changes

- 将 `probe_submit` 从逐小批次准备改成按类型分桶的 batch `PrepRead` 路径，统一生成 `vec_only` / `all` 请求计划并批量发射 SQE。
- 将 submit 策略改成按 cluster 边界触发的 windowed batch submit：每个 probed cluster 结束后，若累计待提交请求数不超过阈值则立即整体提交；若超过阈值，则按 `8` 或 `16` 的固定粒度分批提交。
- 保持现有 `SafeIn` / `Uncertain` 分类语义、`VEC_ONLY` / `ALL` 选择语义、CRC cluster 边界语义和 early-stop 判定边界不变。
- 为 benchmark 增加 submit window 与批量 prepare 的细化统计，便于区分“用户态批量准备收益”和“内核 submit 次数收益”。

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `query-pipeline`: query 在线路径需要支持按 cluster 边界执行 windowed batch submit，并保留现有候选分类与 CRC 语义。
- `async-cluster-prefetch`: 异步预取链路需要从逐批 `PrepRead` 改成按类型分桶的 batch `PrepRead` 与阈值化 submit。
- `e2e-benchmark`: benchmark 需要输出 windowed submit 与 batch prepare 的新增观测项，便于分析 `probe_submit_ms` 与 `uring_submit_ms`。

## Impact

受影响代码主要在 `include/vdb/query/overlap_scheduler.h`、`src/query/overlap_scheduler.cpp`、`include/vdb/query/search_context.h`、`benchmarks/bench_e2e.cpp` 以及相关 OpenSpec 规格文件。CLI 不新增用户参数；submit 阈值先作为实现内固定策略落地，默认候选窗口为 `8` 或 `16` 的固定粒度。
