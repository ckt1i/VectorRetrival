## 1. Stage 1 Kernel Baseline

- [x] 1.1 为 `IPExRaBitQ` 明确保留 reference 路径与新 AVX-512 主路径的边界，确保可以逐 query 对拍 `ip_raw`
- [x] 1.2 在 `ip_exrabitq.cpp` 中把当前 code-centric 数据流拆开，明确 magnitude、sign、bias 三部分的消费边界
- [x] 1.3 为跨维度实现固定 block 策略（如 `64/32/16/tail`），避免只对 `dim=512` 特化

## 2. Stage 1 Kernel Rewrite

- [x] 2.1 实现不改索引格式的 query-centric `IPExRaBitQ` AVX-512 kernel，避免重建完整 signed float(code)
- [x] 2.2 将 `+0.5` bias 项从主积累路径中拆开，明确其与主 dot 的等价关系和实现边界
- [x] 2.3 保留 scalar / reference fallback，并确保新 kernel 与 reference 路径在 `ip_raw` 上等价
- [x] 2.4 在 `ClusterProber` 的 Stage2 路径接入新 kernel，但不改变现有 Stage2 分类与后续语义

## 3. Stage 1 Validation

- [x] 3.1 跑 query-only + `fine_grained_timing=0` clean perf，确认 `IPExRaBitQ` 的热点占比下降
- [x] 3.2 跑 full E2E 验证 recall、avg/p50/p95/p99 与现有路径兼容
- [x] 3.3 回填结果并判断第一阶段后 `IPExRaBitQ` 是否仍为显著主热点

## 4. Stage 2 Decision Gate

- [ ] 4.1 如果第一阶段后 `IPExRaBitQ` 仍显著占用 query CPU，则先设计 `sign bit-pack` 的存储与解析边界
- [ ] 4.2 评估 `sign bit-pack` 对编码、cluster store、parsed cluster 和 migration 的影响
- [ ] 4.3 仅在 `sign bit-pack` 后仍不足时，继续设计 `compact ex-code layout`
- [ ] 4.4 为第二阶段存储格式升级明确 rebuild / migration / rollback 方案

## 5. Benchmark And Perf Gates

- [x] 5.1 固定 clean perf 口径：`query-only + fine_grained_timing=0` 下不得混入 prepare 细粒度打点污染
- [x] 5.2 固定 full E2E 口径：同参数 resident + IVF 路径下回填真实端到端时延与 recall
- [x] 5.3 记录 Stage 1 与 Stage 2 决策结论，明确后续是止步于 kernel 还是继续进入 layout 升级
