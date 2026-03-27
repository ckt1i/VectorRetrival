## Why

Phase 1 (`conann-crc-early-stop`) 已完成 CRC 核心算法的独立验证（CrcStopper、CrcCalibrator、bench_conann_crc）。但 Phase 1 使用精确 L2 距离，未经过 RaBitQ 估计，也未集成到 OverlapScheduler 的 I/O 流水线中。

本变更将 CRC 集成到 OverlapScheduler，同时引入两项优化：
1. **CRC 早停替换 d_k**：用 CrcStopper 替换 `overlap_scheduler.cpp:258-265` 的固定阈值早停
2. **动态 SafeOut 分界线**：将 ConANN.Classify 的 SafeOut 阈值从静态 `d_k` 改为动态 `d_p_k`（当前 RaBitQ estimate heap top），使 SafeOut 随搜索进展越来越激进
3. **CRC 标定使用 RaBitQ 距离**：保证标定和在线推理使用相同的距离空间

## What Changes

- **CrcCalibrator 改造**：`compute_scores` 改用 RaBitQ 估计距离（而非精确 L2），与线上 CrcStopper 的输入空间一致
- **OverlapScheduler 新增 RaBitQ estimate heap**：维护 top-k 的 RaBitQ 近似距离，供 CrcStopper 和动态 SafeOut 使用
- **ConANN 新增 ClassifyAdaptive**：SafeOut 使用动态 `d_p_k + 2·margin`，SafeIn 保持静态 `d_k - 2·margin`
- **Prefetch 参数化**：initial_prefetch=4, refill=2, max_prefetch=16，可配置
- **probe_batch_size 改为 128**
- **Benchmark**：对比旧/新 SafeOut 误杀率、端到端搜索时间、流水线重叠比例

## Capabilities

### New Capabilities

- `dynamic-safeout`: ConANN.ClassifyAdaptive 使用动态 SafeOut 阈值
- `rabitq-crc-calibration`: CRC 标定使用 RaBitQ 估计距离
- `crc-overlap-benchmark`: 端到端性能对比 benchmark

### Modified Capabilities

- `crc-early-stop`: CrcStopper 输入从 exact L2 改为 RaBitQ estimate
- `overlap-scheduler`: 替换 d_k 早停为 CRC 早停 + 动态 SafeOut + prefetch 参数化

## Impact

- `include/vdb/index/conann.h` — 修改：新增 `ClassifyAdaptive()` 方法
- `src/index/conann.cpp` — 修改：实现 `ClassifyAdaptive()`
- `include/vdb/index/crc_calibrator.h` — 修改：接口增加 RaBitQ 相关参数
- `src/index/crc_calibrator.cpp` — 修改：`compute_scores` 使用 RaBitQ 估计距离
- `src/query/overlap_scheduler.cpp` — 修改：CRC 早停 + estimate heap + 动态 SafeOut + prefetch 参数化
- `include/vdb/query/overlap_scheduler.h` — 修改：新增 `CrcStopper` 成员 + estimate heap
- `include/vdb/query/search_context.h` — 修改：SearchConfig 新增 CRC 参数 + prefetch 参数
- `benchmarks/bench_conann_crc.cpp` — 修改：增加端到端对比模式（旧 vs 新 SafeOut、搜索时间、流水线重叠）
- `benchmarks/CMakeLists.txt` — 修改：可能新增 benchmark target
- 现有 `ConANN.Classify(dist, margin)` 保持不变，向后兼容
