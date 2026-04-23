## Why

当前 CRC runtime calibration 的主要耗时集中在 `BrentSolve -> ComputePredictions -> sort` 链路。每次评估候选 `lambda` 时，系统都会对每条 calibration query 重新构造并排序 `(reg_score, step)`，导致 benchmark preparation 阶段出现大规模重复计算，已经成为开启 CRC 后最显著的新热点。

这一步需要先用最小语义改动把 calibration 计算组织方式收敛为“单次预排序 + 多次二分查询”。目标是先压掉重复排序和重复 prediction 重建的成本，同时保持现有 Brent solver、`lamhat` 语义和线上 `CrcStopper` 行为不变，为后续是否切换离散候选阈值搜索保留独立评估空间。

## What Changes

- 将 CRC calibration 的 query-level stop 轨迹预处理为可复用 profile，而不是在每次 `lambda` 评估时重新排序所有 probe step。
- 对每条 calibration query 仅执行一次 `(reg_score, step)` 预排序，后续 Brent 迭代通过二分查找选出该 `lambda` 对应的 stop step。
- 允许 calibration 在预处理阶段预先计算 query/step 对应的 overlap 或等价命中统计，避免 Brent 迭代期间重复构造 prediction 集和集合比对。
- 保留现有 `RegularizeScores`、`PickLambdaReg`、Brent 求解和 `CalibrationResults` 输出接口；本 change 不引入离散候选阈值搜索，不改变 `lamhat` 的定义或对外日志语义。
- 增加 benchmark / calibration 侧的验证口径，要求新实现与旧实现在 `lamhat`、calibration eval 和线上 early-stop 行为上保持等价或近似等价，同时确认 prepare 阶段耗时下降。

## Capabilities

### New Capabilities

### Modified Capabilities
- `crc-calibration`: CRC calibration 必须支持基于预排序 stop profile 的快速 `lambda` 评估，并在保持 Brent solver 语义不变的前提下避免重复排序和重复 prediction 重建。

## Impact

- 主要影响代码位于 `src/index/crc_calibrator.cpp` 及相关头文件中的 calibration 内部数据结构和评估流程。
- 影响 runtime CRC benchmark 的 preparation 成本与 profiling 结果，但不改变线上 `CrcStopper` 的输入输出契约，也不改变索引格式和 `crc_scores.bin` 格式。
- 验证将依赖现有 `bench_e2e` CRC 路径与 perf/profile 口径，重点比较 calibration 时间、`lamhat` 一致性以及在线 recall / early-stop 指标稳定性。
