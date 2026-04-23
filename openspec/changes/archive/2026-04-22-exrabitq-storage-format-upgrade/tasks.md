## 1. Format Definition

- [x] 1.1 为 packed-sign ExRaBitQ 定义新的 `.clu` file version 与对应的 Region2 entry size 计算规则
- [x] 1.2 固化 packed sign 的 bit ordering、entry 字段顺序和旧/新版本 parse 分支边界
- [x] 1.3 在结果输出或元数据路径中增加 ExRaBitQ storage version / format 标识

## 2. Encoder And Cluster Store

- [x] 2.1 调整 `RaBitQCode` / encoder 输出，使 packed sign 成为 ExRaBitQ 的持久化主表示
- [x] 2.2 更新 `ClusterStoreWriter::WriteVectors()`，按新版本写出 `ex_code + packed_sign + xipnorm`
- [x] 2.3 更新 `ClusterStoreReader` 的 entry-size 计算和 parse 逻辑，使其按 file version 区分旧/新 ExRaBitQ 布局
- [x] 2.4 明确旧 reader / 新 reader 对不同版本 `.clu` 的接受、拒绝和错误提示行为

## 3. Resident Parse And Query Path

- [x] 3.1 更新 `ParsedCluster` / resident cluster view，使查询路径可直接访问 packed sign
- [x] 3.2 确保 async parse 与 resident preload 路径不再把 packed sign materialize 为逐维 byte-sign 常驻缓冲
- [x] 3.3 更新 `IPExRaBitQ` 与相关查询接入点，使 Stage2 直接消费 packed sign
- [x] 3.4 验证 packed-sign Stage2 在不同常见维度上的正确性与结果语义一致性

## 4. Benchmark And Perf Validation

- [x] 4.1 更新 `bench_e2e` 输出，显式记录 ExRaBitQ storage version / format provenance
- [ ] 4.2 跑 query-only clean perf，对比旧 byte-sign 与新 packed-sign 的 Stage2 热点迁移
- [ ] 4.3 跑 full E2E，对比 recall、avg/p50/p95/p99 与 Stage2 相关时间字段
- [ ] 4.4 汇总 packed-sign 是否带来真实收益，并判断是否需要进入下一轮 compact layout 优化

## 5. Migration And Rollback Closure

- [x] 5.1 明确新格式索引是否必须 rebuild，并把该边界写入实现说明或输出元数据
- [ ] 5.2 为 serving 回滚场景验证“旧二进制 + 新格式索引”会明确失败而不是静默误解析
- [ ] 5.3 为后续 `compact ex-code layout` 留出清晰扩展位，但不在本 change 内提前混入实现
