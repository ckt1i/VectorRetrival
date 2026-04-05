## 1. IvfBuilder 距离误差校准

- [x] 1.1 修改 CalibrateEpsilonIp: 改为比较 FastScan 距离 vs 真实 L2 距离，归一化为 IP 误差单位
- [x] 1.2 删除 CalibrateEpsilonIpFastScan（不再需要两套校准函数）
- [x] 1.3 更新 IvfBuilder::Build 中的注释，反映新的校准逻辑

## 2. OverlapScheduler ProbeCluster 改造

- [x] 2.1 删除 Popcount 循环（UnpackSignBitsFromFastScan + PopcountXor + pop_dists 计算）
- [x] 2.2 改用 FastScan dists[] 做 S1 ClassifyAdaptive
- [x] 2.3 改用 FastScan dists[] 更新 CRC est_heap

## 3. CRC Calibration 一致性

- [x] 3.1 CrcCalibrator::CalibrateWithRaBitQ 中距离计算改用 FastScan（需引入 FastScan 批处理或 EstimateDistanceFastScan）

## 4. 验证

- [x] 4.1 编译通过
- [x] 4.2 bench_vector_search 对比: recall@10 0.968 vs 0.965, False SafeOut=0 vs 0, latency -27%
- [x] 4.3 bench_e2e --cold --bits 4 --queries 500 功能正确 (recall=1.000), CRC calibration 需调参
