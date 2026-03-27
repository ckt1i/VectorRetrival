# Tasks: ConANN CRC Early Stop

## Phase 1: CRC 核心实现 + Benchmark 验证

### 1. CrcStopper (在线早停判断)

- [ ] 1.1 新增 `include/vdb/index/crc_stopper.h`: `CalibrationResults` 结构体 + `CrcStopper` 类声明
- [ ] 1.2 实现 `CrcStopper::ShouldStop()`: nonconf 归一化 → RAPS 正则化 → λ̂ 比较，O(1)
- [ ] 1.3 单元测试: 边界情况（堆未满、d_max==d_min、probed_count==nlist）

### 2. CrcCalibrator (离线标定)

- [ ] 2.1 新增 `include/vdb/index/crc_calibrator.h` + `src/index/crc_calibrator.cpp`
- [ ] 2.2 实现 `compute_scores()`: 对每条查询逐步 probe 所有聚类，记录 d_p 和预测集
- [ ] 2.3 实现 `全局归一化`: 统计 d_min, d_max
- [ ] 2.4 实现 `RAPS 正则化`: regularize_scores() + pick_lambda_reg() (tune 集选参)
- [ ] 2.5 实现 `Brent 求根`: 使用 GSL `gsl_root_fsolver_brent`，CMakeLists 添加 `find_package(GSL REQUIRED)`
- [ ] 2.6 实现 `Calibrate()` 主入口: 串联 split → compute_scores → normalize → tune → brent → 返回 CalibrationResults
- [ ] 2.7 实现 `evaluate_test()`: 在 test 集上评估 FNR 和平均 probe 数
- [ ] 2.8 单元测试: 使用合成数据验证标定流程正确性

### 3. Benchmark: bench_conann_crc

- [ ] 3.1 新增 `benchmarks/bench_conann_crc.cpp`: 加载 coco npy 数据 → KMeans 聚类 → 计算 GT → CRC 标定 → 评估
- [ ] 3.2 CMakeLists.txt 新增 target
- [ ] 3.3 输出指标: FNR (target vs actual)、平均 probe 数、recall@1/5/10、各 α 下的结果对比
- [ ] 3.4 编译运行，验证 coco 数据集上 CRC 剪枝效果

## Phase 2: OverlapScheduler 集成

### 4. SearchConfig 扩展

- [ ] 4.1 `SearchConfig` 新增 `CalibrationResults* crc_params = nullptr`
- [ ] 4.2 `SearchStats` 新增 `uint32_t crc_probed_count` 字段

### 5. OverlapScheduler 替换 d_k 早停

- [ ] 5.1 在 `OverlapScheduler` 构造时，若 `config_.crc_params` 有效则初始化 `CrcStopper`
- [ ] 5.2 替换 `ProbeAndDrainInterleaved()` L258-265: `d_k` 判断 → `CrcStopper::ShouldStop()`
- [ ] 5.3 保留 `config_.early_stop` 开关兼容性（crc_params == nullptr 时 fallback 到旧 d_k 逻辑）

### 6. 端到端测试

- [ ] 6.1 修改 `bench_e2e` 或新增测试，验证 OverlapScheduler + CRC 的端到端 recall 和延迟
- [ ] 6.2 对比 d_k 早停 vs CRC 早停的 recall / probe 数 / 延迟
