## 1. Fused Prepare Baseline

- [x] 1.1 在 `fastscan.cpp` / `prepare_query.cpp` 中明确参考路径与 fused 路径的边界，保留可对拍的 `QuantizeQuery14BitWithMax -> BuildFastScanLUT` 参考实现
- [x] 1.2 将 `BuildFastScanLUT` 内部逻辑拆成 group LUT、byte plane、packed write 三段，并确保每段可独立验证
- [x] 1.3 为 fused 路径准备最小 scratch / helper 接口，避免完整 `quant_query` 往返成为默认主路径

## 2. Quantize + LUT Fusion

- [x] 2.1 实现 fused `quantize + lut_build` 主路径，支持 block 或寄存器粒度量化后直接生成 LUT
- [x] 2.2 确保 fused 路径生成的 `fs_width`、`fs_shift` 和 packed LUT bytes 与参考路径完全等价
- [x] 2.3 在 `RaBitQEstimator` 中接入 fused 路径，同时保留参考路径作为 debug / regression fallback
- [x] 2.4 将 fused 路径纳入 prepare 子阶段计时，保证 `probe_prepare_quantize_ms` 与 `probe_prepare_lut_build_ms` 仍可同口径比较

## 3. LUT SIMD Deepening

- [x] 3.1 为 `BuildLut16` 的 16-entry group LUT 生成补充 SIMD / block 化实现，优先覆盖一组 codebook 在多个维度块上的并行累加
- [x] 3.2 为 lo/hi byte plane 拆分补充向量化 pack / shift / mask 写出路径，减少逐字节拆包和临时缓冲搬运
- [x] 3.3 为最终 packed lane layout 写出补充 AVX-512 / AVX2 兼容实现路线和 fallback 边界，尽量直接写入最终 FastScan 布局
- [x] 3.4 为 `QuantizeQuery14BitWithMax` 的 pack/store 压缩补充 fused 前端实现路线，减少量化结果落地再重读
- [ ] 3.5 明确 fused 路径与“两段版 + LUT SIMD 化”之间的切换策略和适用条件

## 4. Stage1 Follow-up SIMD Candidates

- [x] 4.1 将 survivor compaction 固化为下一轮 Stage1 的第一优先级 SIMD / batch 优化点，目标是减少 `mask -> branch -> scatter` 的逐候选开销
- [x] 4.2 将 batch classify 固化为第二优先级优化点，按 block 统一完成 SafeIn / Uncertain 判定，并明确它与 `mask` 路径分离验收
- [ ] 4.3 将 `EstimateDistanceFastScan` 记录为第三优先级后续压缩点，重点评估是否存在 lane 并行和更紧凑的累加布局
- [ ] 4.4 将 `DecodeAddressBatch` 记录为后续 SIMD / layout 优化边界，重点评估批量 decode、连续地址展开和分支消除
- [ ] 4.5 将 batch submit 前整理记录为后续 SIMD / layout 优化边界，重点评估批量去重、slot 准备和读请求组织
- [ ] 4.6 记录当前不建议优先投入的点，例如 `FastScanSafeOutMask` 深挖、MKL 路线和 rerank/payload 尾段，并写清原因

## 5. Benchmark And Perf Gates

- [ ] 5.1 固定 fused 路径实现后的 full E2E 验收项：recall、avg/p50/p95/p99、prepare/stage1 全部细分字段
- [ ] 5.2 固定 fused 路径实现后的 query-only perf 验收项：`lut_build` 热点占比、Stage1 survivor 后处理占比
- [ ] 5.3 要求每轮实现后回填结果，并据此判断下一步优先做 LUT 深化还是 Stage1 survivor 路径
