# Tasks: Fix d_k Calibration — Query/Database Separation

## 1. ConANN::CalibrateDistanceThreshold 新重载

- [x] 1.1 在 `conann.h` 新增重载声明: `CalibrateDistanceThreshold(queries, Q, database, N, dim, ...)`
- [x] 1.2 在 `conann.cpp` 实现新重载: 从 queries 采样，搜索 database，无 self-distance 处理
- [x] 1.3 将旧签名改为 wrapper: 内部调用新重载，传 `queries = database = vectors`

## 2. IvfBuilderConfig + CalibrateDk 适配

- [x] 2.1 在 `IvfBuilderConfig` 新增 `const float* calibration_queries = nullptr` 和 `uint32_t num_calibration_queries = 0`
- [x] 2.2 修改 `IvfBuilder::CalibrateDk`: 若 config 有 query 样本则调用新重载，否则 fallback 旧行为

## 3. Benchmark 修正

- [x] 3.1 `bench_rabitq_accuracy` Phase 3: 改为 `CalibrateDistanceThreshold(qry, Q, img, N, ...)`
- [x] 3.2 `bench_e2e`: 如有 query 数据，设置 `cfg.calibration_queries` 传入 Build

## 4. 测试更新

- [x] 4.1 `ivf_builder_test`: 新增测试用例验证带 calibration_queries 的 Build 行为
- [x] 4.2 现有测试全部通过（默认 nullptr fallback 不受影响）

## 5. 编译验证 + 运行 benchmark

- [x] 5.1 编译无 warning
- [x] 5.2 运行 ctest 全部通过 (35/35)
- [x] 5.3 运行 `bench_rabitq_accuracy --dataset coco_1k`，d_k=1.529 (qry→img)，SafeOut=14.87%，False SafeOut=0%
