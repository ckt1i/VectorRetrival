# 提案：将 CRC 标定与距离空间解耦

## 问题

`CrcCalibrator` 当前存在两个关联问题：

1. **跨空间不匹配**：`CalibrateWithRaBitQ` 在 RaBitQ 空间计算分数，却与精确 L2 的 ground truth 进行比较。RaBitQ top-K 与精确 L2 top-K 的重叠率仅 30–50%，导致 FNR 始终 ≥50%，BrentSolve 返回 λ̂ = 1.0，early-stop 完全不触发。

2. **紧耦合**：CrcCalibrator 内部包含两套独立的搜索实现（`ComputeScoresForQuery` 用于精确 L2，`ComputeScoresForQueryRaBitQ` 用于 RaBitQ）。它依赖原始向量、RaBitQ 编码、旋转矩阵、质心——但统计算法真正需要的只是 `QueryScores`。

## 方案

### A. 解耦：提取纯分数标定 API

CRC 标定的统计核心（归一化 → 正则化 → BrentSolve）只需要 `QueryScores`——每个查询一组 `(raw_scores[nlist], predictions[nlist][K])`。提取新的 `Calibrate(scores, nlist)` API，接收预计算的分数。Ground truth 从 `predictions[nlist-1]`（全探测结果）自动派生。

现有的 `Calibrate(精确 L2)` 和 `CalibrateWithRaBitQ` 变为薄封装：计算分数 → 调用核心。

### B. 离线化：在构建阶段计算并存储分数

将分数计算移入 `IvfBuilder::Build()`。索引构建完成后，用标定查询集进行全探测搜索，将 `QueryScores` 序列化到二进制文件（`crc_scores.bin`）。查询时加载分数文件即可立即完成标定——无需原始向量，无需重新搜索。

这带来：
- **秒级重标定**：更换 α 值不需要重新搜索（分钟级 → 秒级）
- **无原始向量依赖**：标定时不需要加载原始向量数据
- **量化方法无关**：任何能产出 `QueryScores` 的距离函数都适用

## 范围

- 新增 `CrcCalibrator::Calibrate(vector<QueryScores>, nlist)` 核心 API
- `QueryScores` 序列化/反序列化工具函数
- `IvfBuilder` 在构建阶段计算并存储 CRC 分数
- FlatBuffers schema：新增 `CrcParams` 表及分数文件引用
- 调用方更新：`bench_e2e.cpp`、`bench_crc_overlap.cpp`
- 单元测试

## 不在范围内

- Rerank 阶段改进
- `CrcStopper` 或在线查询路径的改动
- 删除便利封装（`Calibrate(精确 L2)`、`CalibrateWithRaBitQ`）——保留给 benchmark 使用
