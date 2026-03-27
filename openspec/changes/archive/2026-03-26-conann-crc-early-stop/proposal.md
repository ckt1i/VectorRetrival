## Why

当前 IVF 搜索的早停机制基于 `d_k` 阈值（`TopDistance() < d_k`），本质是一个固定阈值的启发式判断，无法在给定置信水平下提供 recall 保证。ConANN 论文提出了基于 Conformal Risk Control (CRC) 的自适应 nprobe 方案：离线标定 λ̂，在线查询时逐步 probe 聚类并计算正则化 nonconformity score，当 `reg_score > λ̂` 时停止，理论保证 FNR ≤ α。

需要在 VDB 中引入 CRC 框架，替换当前的 `d_k` 早停，同时保留现有的 RaBitQ 三分类（SafeIn/SafeOut/Uncertain）作为向量级剪枝。

## What Changes

- **新增 CRC 标定器** (`CrcCalibrator`)：离线标定流程，输入校准查询集 + IVF 索引 + GT，输出 `CalibrationResults { λ̂, k_reg, λ_reg, d_min, d_max }`
- **新增 CRC 早停判断** (`CrcStopper`)：在线查询时，每 probe 一个聚类后计算 `reg_score`，与 `λ̂` 比较决定是否停止
- **Benchmark**：`bench_conann_crc` — 自包含的 IVF + CRC + 精确 L2 距离测试，验证 CRC 在 coco 数据集上的剪枝效果
- **OverlapScheduler 集成**：替换 `overlap_scheduler.cpp:258-265` 的 `d_k` 早停为 CRC 早停

## Capabilities

### New Capabilities

- `crc-calibration`: CRC 离线标定流程（全局 d_min/d_max 归一化、RAPS 正则化、Brent 求根）
- `crc-early-stop`: CRC 在线早停判断（聚类级自适应 nprobe）

### Modified Capabilities

- `per-cluster-epsilon`: OverlapScheduler 中 `d_k` 早停替换为 CRC 早停

## Impact

- `include/vdb/index/crc_calibrator.h` — 新增：CRC 标定器
- `src/index/crc_calibrator.cpp` — 新增：标定实现（Brent 求根、RAPS 正则化）
- `include/vdb/index/crc_stopper.h` — 新增：CRC 早停判断
- `src/query/overlap_scheduler.cpp` — 修改：替换 d_k 早停为 CRC 早停
- `include/vdb/query/search_context.h` — 修改：SearchConfig 新增 CRC 参数
- `benchmarks/bench_conann_crc.cpp` — 新增：Phase 1 验证 benchmark
- `benchmarks/CMakeLists.txt` — 修改：新增 benchmark target
- 现有 RaBitQ 三分类（`conann.h`）不受影响，向量级剪枝保持不变
