# 任务：将 CRC 标定与距离空间解耦

## 1. 公开 QueryScores 与核心 API

- [x] 1.1 将 `QueryScores` 从 `crc_calibrator.cpp` 匿名命名空间移至 `include/vdb/index/crc_calibrator.h` 作为公开结构体
- [x] 1.2 在 `CrcCalibrator` 中新增 `Calibrate(vector<QueryScores>, nlist)` 静态方法 —— 与距离空间无关的核心。内部从 `predictions[nlist-1]` 派生 GT，执行归一化 → 正则化 → BrentSolve → 评估
- [x] 1.3 重构现有 `Calibrate(精确 L2)`：计算分数 → 调用核心 `Calibrate(scores, nlist)`
- [x] 1.4 重构 `CalibrateWithRaBitQ`：计算分数 → 调用核心。移除 `ground_truth` 参数
- [x] 1.5 更新 `EvalResults` 文档注释：指标衡量搜索空间自一致性，而非与精确 L2 的比较

## 2. QueryScores 序列化

- [x] 2.1 新增 `WriteScores(path, scores, nlist, top_k)` 和 `ReadScores(path, scores, nlist, top_k)` 工具函数（放在 `crc_calibrator.h`/`.cpp`）
- [x] 2.2 二进制格式：头部（magic `CRCS`、版本号、num_queries、nlist、top_k）+ 每查询（raw_scores[nlist] + predictions[nlist × top_k]，不足用 UINT32_MAX 填充）

## 3. IvfBuilder 集成

- [x] 3.1 在 `IvfBuilder::Build()` 的 `WriteIndex()` 之后新增 CRC 分数预计算阶段：用 RaBitQ 全探测标定查询集，序列化到 `crc_scores.bin`
- [x] 3.2 更新 FlatBuffers schema（`segment_meta.fbs`）：新增 `CrcParams` 表，含 `scores_file`、`num_calibration_queries`、`calibration_top_k`；在 `SegmentMeta` 中新增 `crc_params` 字段
- [x] 3.3 在 `WriteIndex()` 中将 `CrcParams` 写入 `segment.meta`

## 4. 调用方更新

- [x] 4.1 更新 `bench_e2e.cpp` CRC 路径：加载 `crc_scores.bin`，调用 `Calibrate(scores, nlist)`。移除 CRC 暴力 GT 计算、CRC 相关的 `all_vectors` 加载、`crc_gt`
- [x] 4.2 更新 `bench_crc_overlap.cpp`：调用新 API，移除 `gt_topk` 参数

## 5. 测试

- [x] 5.1 `WriteScores`/`ReadScores` 往返测试
- [x] 5.2 更新 `crc_calibrator_test.cpp` 以适配新签名
- [x] 5.3 构建所有目标：`cmake --build build`

## 6. 验证

- [ ] 6.1 在 coco_1k 上运行 `bench_e2e --crc`：验证 λ̂ < 1.0，early-stop 触发，avg_probed < nlist
- [ ] 6.2 在 coco_5k 上运行 `bench_e2e --crc`：验证同上
- [ ] 6.3 验证构建过程中 `crc_scores.bin` 被写入索引目录
