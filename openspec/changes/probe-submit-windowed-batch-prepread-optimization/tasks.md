## 1. Batch PrepRead 骨架

- [ ] 1.1 在 `AsyncIOSink` 中引入按类型分桶的请求收集结构，分别承载 `vec_only` 与 `all` 候选
- [ ] 1.2 新增批量读取计划生成路径，将地址、长度、slot/buffer 绑定与请求类型整理为 batch read plan
- [ ] 1.3 新增批量 SQE 发射路径，并保持底层 `VEC_ONLY` / `ALL` 语义与当前实现一致
- [ ] 1.4 将 `probe_submit_ms` 的统计拆到 batch prepare / batch emit 边界，确保用户态准备成本可单独观察

## 2. Cluster-End Windowed Submit

- [ ] 2.1 在 cluster 结束时引入 submit window 决策逻辑，以累计待提交请求数作为阈值判断依据
- [ ] 2.2 实现“小于等于阈值则整体提交、大于阈值则按固定粒度分批提交”的默认策略
- [ ] 2.3 将固定 window 粒度先实现为 `16` 或 `32` 的可切换常量，并确保不会破坏现有 CRC cluster 边界与 early-stop 语义
- [ ] 2.4 校验 flush、tail flush 与 ring 容量边界，避免因为窗口化提交导致请求滞留或遗漏

## 3. Benchmark 与可观测性

- [ ] 3.1 在 `SearchStats` / benchmark 聚合结果中增加 submit window 统计项，如 flush 次数、平均每次 flush 请求数、tail flush 次数
- [ ] 3.2 在 benchmark 输出中增加 batch `PrepRead` 统计项，至少区分 `vec_only` prepare、`all` prepare、batch emit 与 `uring_submit`
- [ ] 3.3 更新 `bench_e2e` 的日志与 `results.json` 输出，确保新字段在 resident 主验收路径下稳定可见

## 4. 回归与性能验收

- [ ] 4.1 使用固定 2000-query resident 参数回归一次 full E2E，确认 recall@1/@5/@10 与当前基线一致
- [ ] 4.2 对比优化前后 `probe_submit_ms`、`uring_submit_ms`、`submit_calls`、`prefetch_wait_ms` 与 `unique_fetch`，确认收益来自 submit 链路而非候选漏斗变化
- [ ] 4.3 在同参数下补跑 query-only perf，确认热点从碎片化 submit / prepare 路径回落
