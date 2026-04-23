## 1. Packed-sign Single-Candidate Baseline Cleanup

- [x] 1.1 保留当前 packed-sign 单 candidate kernel 作为 reference fast path，并固定其作为 true batch kernel 的正确性对拍基线
- [x] 1.2 保持 `IPExRaBitQ` 兼容入口、reference / fallback 与现有 resident serving 语义不变

## 2. True Multi-Candidate Kernel

- [x] 2.1 在 `include/vdb/simd/ip_exrabitq.h` / `src/simd/ip_exrabitq.cpp` 中实现 `batch=8` 的 packed-sign true multi-candidate kernel
- [x] 2.2 kernel 采用通用 `64/32/16/tail` block 结构，不做 `dim=512` 特化
- [x] 2.3 kernel 必须在同一个 dim block 内同时累积多个 candidate，复用 query block，不允许退化成循环调用单 candidate kernel
- [x] 2.4 为 true batch kernel 保留数值一致性对拍，确认与单 candidate fast path / reference 结果一致

## 3. Stage2 Query-Path Integration

- [x] 3.1 在 `src/index/cluster_prober.cpp` 中新增 Stage2 batch candidate scratch，固定第一版 batch 大小为 `8`
- [x] 3.2 将 Stage2 从 `for each uncertain candidate -> IPExRaBitQ(...)` 改为 `collect uncertain -> IPExRaBitQBatchPackedSign(...)`
- [x] 3.3 保持 Stage1 funnel、top-k 结果、CRC、payload 和 resident serving 语义不变
- [x] 3.4 尾批次与非 packed-sign 路径走兼容回退，不改变现有索引格式和 cluster store 持久化格式

## 4. Validation

- [x] 4.1 跑 query-only + `fine_grained_timing=0` clean perf，确认 `IPExRaBitQ` 占比与 `avg_query` 的变化
- [x] 4.2 跑同口径 full E2E，确认 recall 与 avg/p50/p95/p99 与现有路径兼容
- [x] 4.3 汇总“单 candidate kernel”与“true multi-candidate kernel”前后的结果差异，判断 true batch kernel 是否达到预期

## 5. Follow-up Decision Gate

- [x] 5.1 若 true batch kernel 后 Stage2 仍显著受 sign/code 带宽限制，再单独整理下一轮 `compact layout / rebuild` 路线
- [x] 5.2 保持 benchmark 当前 `fine_grained_timing=0` 的低开销正式口径，不引回细粒度打点污染
