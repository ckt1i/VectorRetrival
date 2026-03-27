# Tasks: CRC E2E Benchmark

## 1. CMakeLists.txt 链接更新

- [x] 1.1 `benchmarks/CMakeLists.txt`: bench_e2e 新增 `vdb_crc` 链接依赖

## 2. bench_e2e CRC 对比模式

### 2.1 命令行参数

- [x] 2.1.1 新增 `--crc` 开关（int，默认 0），控制是否启用 CRC 对比模式
- [x] 2.1.2 新增 `--crc-alpha`、`--crc-calib`、`--crc-tune` 参数

### 2.2 Phase C.5: 磁盘 ClusterData 加载

- [x] 2.2.1 在 Phase C（Build）之后、Phase D（Query）之前，新增 Phase C.5
- [x] 2.2.2 实现磁盘加载：
  - 对每个 cluster: `Segment.EnsureClusterLoaded(cid)` → `GetCodePtr(cid, 0)` 获取 codes_block
  - 从 `GetAddress(cid, i)` + `Segment.ReadVector()` 获取原始向量
  - 组装 `ClusterData[]`（vectors, ids=global_offset, codes_block, code_entry_size）
- [x] 2.2.3 合并所有 cluster 向量，分配 global offset 作为 vector ID

### 2.3 Phase C.5: CRC 标定

- [x] 2.3.1 从磁盘加载的向量重新计算 brute-force GT（uint32_t global offset IDs）
- [x] 2.3.2 调用 `CrcCalibrator::CalibrateWithRaBitQ()` 得到 `CalibrationResults`
- [x] 2.3.3 打印标定结果：d_min, d_max, lamhat, k_reg, lam_reg, EvalResults

### 2.4 Phase D 扩展: 双轮查询

- [x] 2.4.1 Round 1（baseline）: `SearchConfig.crc_params = nullptr`，执行所有查询
- [x] 2.4.2 Round 2（CRC）: `SearchConfig.crc_params = &calib_results`，`initial_prefetch = 4`，执行所有查询
- [x] 2.4.3 收集两轮的 QueryResult（提取 RunQueryRound 辅助函数避免重复）

### 2.5 Phase E 扩展: 对比评估

- [x] 2.5.1 分别计算两轮的 recall@1/5/10
- [x] 2.5.2 分别计算两轮的时间指标（avg/p50/p95/p99）
- [x] 2.5.3 分别计算两轮的流水线指标（SafeOut/SafeIn/Uncertain 平均值、早停率、avg_probed）
- [x] 2.5.4 计算 overlap_ratio = 1 - avg_io_wait / avg_total_time
- [x] 2.5.5 打印对比表格（Baseline vs CRC 并排）

### 2.6 Phase F 扩展: JSON 输出

- [x] 2.6.1 results.json 中新增 `"crc_comparison"` 段，包含两轮所有指标
- [x] 2.6.2 config.json 中新增 CRC 参数信息

## 3. 编译验证

- [x] 3.1 确保 `--crc 0`（默认）时 bench_e2e 行为与改动前完全一致
- [x] 3.2 确保 `--crc 1` 时编译通过
- [ ] 3.3 确保现有 bench_e2e 测试和其他 benchmark 不受影响

## 4. 运行验证

- [ ] 4.1 在 coco_1k 上运行 `bench_e2e --crc 1`，检查：
  - CRC 标定输出合理（d_min < d_max, lamhat > 0）
  - CRC 模式 recall 不低于 baseline 2% 以上
  - CRC 模式 SafeOut 率高于 baseline
  - 搜索时间和 overlap_ratio 有改善或至少不退化
