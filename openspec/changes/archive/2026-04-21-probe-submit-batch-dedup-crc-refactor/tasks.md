## 1. Batched Submit Skeleton

- [x] 1.1 为 `AsyncIOSink` 设计固定容量 batch scratch，承接 `CandidateBatch` 的扫描、分桶和暂存状态
- [x] 1.2 将 `OnCandidates(batch)` 改造成阶段化 batch submit 流程，替代逐 candidate `SubmitOne()` 主路径
- [x] 1.3 在 batched submit skeleton 中保持现有 `VEC_ALL` / `VEC_ONLY` 选择语义和当前 slot / buffer 生命周期语义

## 2. Batch-Aware Dedup

- [x] 2.1 在 `CandidateBatch` 内实现 batch-local offset 去重，并补充对应统计
- [x] 2.2 将 query 级 dedup 从当前通用容器收敛为更轻的 query-local dedup 结构
- [x] 2.3 验证 batch-local dedup 与 query-global dedup 的组合不会改变提交语义

## 3. CRC And Timing Off Hotpath

- [x] 3.1 增加 cluster-local CRC estimate 收集，并在 cluster-end merge 到 query-level CRC 状态
- [x] 3.2 将 submit-path 微计时从 per-candidate 收敛为 batch/group 级计时
- [x] 3.3 保持 `probe_submit_ms` 与 `uring_prep_ms` 的对比可解释性

## 4. Validation

- [x] 4.1 使用固定 operating point 跑一轮 full E2E，确认 recall 与提交相关统计不异常回退
- [x] 4.2 使用同参数 query-only perf 复核 submit-path 热点是否下降并记录热点迁移
- [x] 4.3 汇总三步改造前后的 `avg_query_ms`、`probe_submit_ms` 与关键 candidate 计数字段
