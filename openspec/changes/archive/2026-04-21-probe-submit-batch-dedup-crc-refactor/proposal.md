## Why

`probe_submit` 已经成为 query 主路径里最明确的次级 CPU 热点之一。当前提交路径虽然接收的是 `CandidateBatch`，但实际仍按逐 candidate 的方式执行 dedup、slot 分配、buffer 获取、`PrepRead*` 和 CRC/timing 维护，导致常数项偏重且难以继续收敛。

## What Changes

- 将 `AsyncIOSink::OnCandidates(batch)` 从“batch 输入 + 逐条 `SubmitOne()`”改成真正的 batch submit skeleton。
- 将 dedup 从 `unordered_set + per-candidate insert` 改造成 batch-aware 路径，包含 batch-local 去重与更轻的 query 级 dedup 结构。
- 将 CRC heap 更新与 per-candidate 微计时从 submit 热路径中拆出，改成 cluster-end merge 与 batch/group 级计时。
- 约束这一轮改造只改变提交组织方式，不改变 SafeIn / Uncertain 分类、rerank、payload、recall 和最终排序语义。
- 为 `probe_submit` 改造补齐稳定的 benchmark / perf 验收口径，便于与现有主路径拆分字段对齐比较。

## Capabilities

### New Capabilities
- `probe-submit-pipeline`: 定义 `probe_submit` 路径的 batch submit、batch-aware dedup、CRC/timing off hotpath 的行为要求。

### Modified Capabilities
- `query-pipeline`: 查询主路径需要对 `probe_submit` 的提交边界、dedup 语义与 cluster 粒度 CRC 更新行为给出稳定约束。
- `e2e-benchmark`: benchmark 输出需要继续保留并验证 `probe_submit` 相关时间与计数口径。

## Impact

主要影响 [overlap_scheduler.h](/home/zcq/VDB/VectorRetrival/include/vdb/query/overlap_scheduler.h)、[overlap_scheduler.cpp](/home/zcq/VDB/VectorRetrival/src/query/overlap_scheduler.cpp)、可能涉及 query-local scratch / dedup 结构和 benchmark 输出解释。当前方案不引入新依赖，不改变 CLI，不改变索引格式。
