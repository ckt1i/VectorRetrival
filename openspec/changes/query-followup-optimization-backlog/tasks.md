## 1. Backlog Baseline

- [ ] 1.1 整理当前 `prepare/stage1` 之外的剩余热点，并记录到 backlog change
- [ ] 1.2 为每个热点补充当前热点归因、预估收益区间和推荐优先级
- [ ] 1.3 固定后续逐项优化必须沿用的 full E2E + query-only perf 口径

## 2. Probe Submit First-Round Packages

- [ ] 2.1 将 `OnCandidates(batch)` 改成真正的 batch submit 记为第一优先级工作包
- [ ] 2.2 为 batch submit skeleton 补充边界约束、改动落点和 `-0.015 ~ -0.030 ms` 的预估收益
- [ ] 2.3 将 batch-aware dedup 记为第二优先级工作包
- [ ] 2.4 为 batch-aware dedup 补充 batch-local 去重、query 级 dedup 结构替换和 `-0.006 ~ -0.015 ms` 的预估收益
- [ ] 2.5 将 CRC heap 更新和微计时移出 submit hotpath 记为第三优先级工作包
- [ ] 2.6 为 CRC/timing off hotpath 补充 cluster-end merge、batch/group 计时边界和 `-0.005 ~ -0.012 ms` 的预估收益

## 3. Other Candidate Optimization Items

- [ ] 3.1 记录 `coarse_select` 后续收敛候选与是否进入 top-n 改写的决策门
- [ ] 3.2 记录 `probe_stage2 / IPExRaBitQ` 的常量项压缩候选
- [ ] 3.3 记录 prefetch / `io_uring` 提交与等待链路的优化候选
- [ ] 3.4 记录 rerank / payload 后段在后续轮次中的重评估条件

## 4. Follow-up Workflow

- [ ] 4.1 为下一轮准备优先从三项 `probe_submit` 工作包中选择单个点继续细化
- [ ] 4.2 约定每完成一项优化后都回填 benchmark / perf 结果并更新优先级
