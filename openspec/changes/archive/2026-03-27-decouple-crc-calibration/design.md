# 设计：将 CRC 标定与距离空间解耦

## 概述

将 CrcCalibrator 重构为纯分数统计核心，与距离计算方法完全无关。在构建阶段计算并持久化 `QueryScores`，使标定可以离线运行，无需原始向量。

## 架构

```
重构前：
  ┌───────────────────────────────────────────────────────┐
  │                CrcCalibrator                           │
  │  ┌─────────────────────────────────────────────────┐  │
  │  │ ComputeScoresForQuery (精确 L2)                 │  │
  │  │ ComputeScoresForQueryRaBitQ (RaBitQ)            │  │ ← 搜索耦合在内部
  │  ├─────────────────────────────────────────────────┤  │
  │  │ 归一化 → 正则化 → BrentSolve                     │  │ ← 纯统计算法
  │  └─────────────────────────────────────────────────┘  │
  │  输入: queries, vectors, codes, centroids, GT         │
  └───────────────────────────────────────────────────────┘

重构后：
  ┌──────────────────────────┐
  │  IvfBuilder (构建阶段)    │
  │  ┌──────────────────────┐│      ┌────────────────────┐
  │  │ 全探测搜索            ││─────▶│  crc_scores.bin    │
  │  │ → QueryScores[Q]     ││      │  (序列化)           │
  │  └──────────────────────┘│      └────────┬───────────┘
  └──────────────────────────┘               │ 加载
                                             ▼
  ┌───────────────────────────────────────────────────────┐
  │                CrcCalibrator                           │
  │  Calibrate(vector<QueryScores>, nlist, config)        │
  │  → GT = predictions[nlist-1] (自动派生)                │
  │  → CalibrationResults { λ̂, kreg, λ_reg, d_min, d_max}│
  └───────────────────────────────────────────────────────┘
```

## 数据模型

### QueryScores（原内部结构体，现公开）

```cpp
struct QueryScores {
    std::vector<float> raw_scores;                  // [nlist] 每步的 kth 距离
    std::vector<std::vector<uint32_t>> predictions; // [nlist][≤K] 每步的 top-K ID
};
```

每个查询：`nlist` 个 float + `nlist × K` 个 uint32。
总量：`Q × nlist × (4 + K × 4)` 字节。示例：Q=500, nlist=32, K=10 → ~688 KB。

### Ground Truth 派生

查询 `i` 的 GT = `scores[i].predictions[nlist - 1]` —— 全探测结果。无需外部 GT 参数。

## 变更详情

### 1. 新核心 API：`CrcCalibrator::Calibrate(scores)`

```cpp
/// 核心标定 —— 与距离空间完全无关。
/// GT 从 predictions[nlist-1] 自动派生。
static std::pair<CalibrationResults, EvalResults> Calibrate(
    const Config& config,
    const std::vector<QueryScores>& all_scores,
    uint32_t nlist);
```

实现：同样的统计流水线（拆分 → 归一化 → 正则化 → BrentSolve → 评估），但直接操作预计算的分数。`ground_truth` 参数被消除；GT 从每个查询的 `predictions[nlist-1]` 提取。

### 2. 便利封装（保留给 benchmark 使用）

现有 `Calibrate(精确 L2)` 和 `CalibrateWithRaBitQ` 变为封装：
1. 通过各自的搜索函数计算 `QueryScores`
2. 调用核心 `Calibrate(scores, nlist)`

两个封装同样移除 `ground_truth` 参数，因为 GT 是自动派生的。

### 3. QueryScores 序列化

`crc_scores.bin` 二进制格式：

```
头部：
  magic: uint32 = 0x43524353  ("CRCS")
  version: uint32 = 1
  num_queries: uint32
  nlist: uint32
  top_k: uint32

每个查询（重复 num_queries 次）：
  raw_scores: float[nlist]
  predictions: uint32[nlist × top_k]    // 固定 K，不足用 UINT32_MAX 填充
```

工具函数：
```cpp
namespace crc {
    Status WriteScores(const std::string& path,
                       const std::vector<QueryScores>& scores,
                       uint32_t nlist, uint32_t top_k);
    Status ReadScores(const std::string& path,
                      std::vector<QueryScores>& scores,
                      uint32_t& nlist, uint32_t& top_k);
}
```

### 4. IvfBuilder 集成

在 `WriteIndex()` 完成后（所有编码、质心、旋转矩阵已写入），新增阶段：

```
Phase D：CRC 分数预计算
  1. 加载标定查询集（来自配置或跨模态查询集）
  2. 对每个查询，使用 RaBitQ 估计进行全探测搜索
     （复用 ComputeScoresForQueryRaBitQ 逻辑）
  3. 序列化 QueryScores → output_dir/crc_scores.bin
```

IvfBuilder 此时已有质心、编码（刚写入）和旋转矩阵。标定查询来自用户提供的查询集（与当前 `CalibrateDk` 使用的相同）。

### 5. 调用方更新

**`bench_e2e.cpp`**：
- CRC 路径：从索引目录加载 `crc_scores.bin`，调用 `Calibrate(scores, nlist)`
- 移除：CRC 暴力 GT 计算（~648–668 行），CRC 相关的 `all_vectors` 加载，`crc_gt`
- 保留：Phase B 的精确 L2 GT（用于 recall 报告）

**`bench_crc_overlap.cpp`**：
- 类似：从文件加载分数或内联计算，调用新 API
- 移除 `gt_topk` 标定调用参数

### 6. FlatBuffers Schema

在 `SegmentMeta` 中新增 CRC 分数文件引用：

```flatbuffers
table CrcParams {
    scores_file: string;               // 相对路径："crc_scores.bin"
    num_calibration_queries: uint32;
    calibration_top_k: uint32;
}
```

在 `SegmentMeta` 中新增 `crc_params: CrcParams;` 字段。

### 7. EvalResults 语义

变更后，`EvalResults` 指标衡量的是搜索空间内的自一致性（early-stop 是否遗漏了全探测能找到的向量？），而非与精确 L2 的准确度。更新文档注释。

## 决策记录

| ID | 决策 | 理由 |
|----|------|------|
| D1 | 纯分数核心 API | 将标定与距离计算完全解耦 |
| D2 | GT 从 `predictions[nlist-1]` 派生 | 全探测是任何搜索能给出的最优结果；自一致的基准 |
| D3 | 二进制文件存储分数 | 简单、快速 IO；体积小（KB–MB 级）；无需 FlatBuffers 开销 |
| D4 | 在 IvfBuilder 中计算分数 | 构建阶段是自然时机；所有数据都可用 |
| D5 | 保留便利封装 | benchmark 需要内联计算；封装只是调用核心 |
| D6 | 序列化使用固定 K 填充 | 避免变长记录；不足 K 个用 UINT32_MAX 哨兵值 |
