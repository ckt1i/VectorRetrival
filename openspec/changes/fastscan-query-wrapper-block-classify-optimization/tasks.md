## 1. Query Wrapper 重构

- [x] 1.1 为 resident 查询路径引入 query-global wrapper / cluster-local prepared view 的内部结构，明确 `rotated_q`、prepare scratch 和 prepared view 的生命周期边界。
- [x] 1.2 重构 `PrepareQueryRotatedInto` 的调用链，使 resident 模式下的多个 cluster probe 复用同一个 query wrapper，而不是重复构造胖 `PreparedQuery` 语义。
- [x] 1.3 保持 Stage1 / Stage2 语义等价，并确认新的 prepared view 足以支撑 FastScan 与 ExRaBitQ 路径。

## 2. Prepare 主路径压缩

- [x] 2.1 固定 prepare 子步骤边界：`subtract_norm`、`normalize_sign_sum_maxabs`、`quantize`、`lut_build`，并确保这些字段可继续观测。
- [x] 2.2 将 `PrepareQueryRotatedInto` 收敛为 query-wrapper 驱动的两段 prepare 流程，减少不必要的中间态保留和重复搬运。
- [x] 2.3 为后续 fused `quantize + LUT build` 预留稳定接口，并确认当前实现输出的 `fs_width`、`fs_shift` 和 LUT bytes 与参考路径等价。
- [x] 2.4 将 prepare 的所有权边界明确到代码结构：`PreparedQuery` 作为兼容壳，`ClusterPreparedScratch` 作为复用 scratch，`PreparedClusterQueryView` 作为 per-cluster 轻量 view；确保 `Probe` 接口不再依赖完整 `PreparedQuery` 状态。

## 3. Stage1 Block-Driven Classify

- [x] 3.1 在 `ClusterProber` 中引入 block-local candidate batch，先对 survivor 做 compact，再进行 classify 和后续处理。
- [x] 3.2 为 `ProbeResultSink` 增加 batch 消费接口，并在 resident + single-assignment 路径上接入 batch candidate submit。
- [x] 3.3 为 `ParsedCluster` 增加 batch address decode 接口，并把地址批量解码纳入 Stage1 后半段正式边界。
- [x] 3.4 确认 block-driven classify 后 recall、排序和候选语义与参考逐候选路径保持一致。
- [x] 3.5 将 Stage1 的语义归因字段与新 prepare 边界统一：`probe_stage1_estimate/mask/iterate/classify` 与 `probe_submit` 不交叉计入，确保后续 clean perf 与 full E2E 可直接对比。

## 4. Benchmark 与 Profiling 口径

- [x] 4.1 保持 `full E2E` 的细粒度解释模式，继续输出 prepare / Stage1 / submit 相关字段。
- [x] 4.2 保持 `query-only clean perf` 的低干扰模式，明确 `fine_grained_timing` 的职责边界并写入输出。
- [x] 4.3 在实现完成后，按固定 resident 参数重跑一轮 full E2E 和一轮同参数 query-only perf。

## 5. 第二轮 SIMD 候选复盘

- [ ] 5.1 基于新主路径重新分析 `PrepareQueryRotatedInto` 下游的 `quantize` / `lut_build` 是否已经稳定到适合继续做 fused SIMD。
- [ ] 5.2 基于 block-driven classify 的结果复盘 `safein/safeout mask`、candidate compaction、address batch decode 和 batch submit 前整理，判断哪些边界已适合作为下一轮 SIMD 目标。
- [ ] 5.3 汇总优化前后 full E2E 与 clean perf 的结果，形成下一轮 SIMD 优先级结论。
