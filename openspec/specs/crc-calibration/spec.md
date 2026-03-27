# CRC Calibration

## Description

ConANN CRC 离线标定流程：给定校准查询集、IVF 索引和 ground truth，输出 `CalibrationResults`，供在线查询早停使用。

## Requirements

### 输入

- 校准查询集 Q_cal: `[n_queries × dim]` float 数组
- IVF 索引: nlist 个聚类的质心 + 每个聚类的向量数据
- Ground truth: 每条查询的真实 top-k 邻居 ID
- 参数: `k` (top-k), `α` (目标 FNR 上界), `dim` (维度)

### 输出

```cpp
struct CalibrationResults {
    float lamhat;        // λ̂
    uint32_t kreg;       // k_reg
    float reg_lambda;    // λ_reg
    float d_min;         // 全局最小 d_p
    float d_max;         // 全局最大 d_p
};
```

### 流程

1. **划分**: Q_cal → calib (50%) / tune (10%) / test (40%)
2. **compute_scores**: 对 calib + tune 集中每条查询:
   - 将 nlist 个聚类按到质心距离排序
   - 逐步 probe 第 1..nlist 个聚类，维护 top-k 堆
   - 记录每步的 `d_p(q)` (当前第 k 近距离) 和 top-k 预测集
3. **全局归一化**: `d_min = min(所有 d_p)`, `d_max = max(所有 d_p)`
4. **nonconformity score**: `nonconf(q, p) = clamp((d_p - d_min) / (d_max - d_min), 0, 1)`
5. **选择 (k_reg, λ_reg)**: 在 tune 集上遍历候选组合 `λ_reg ∈ {0, 0.001, 0.01, 0.1}`，`k_reg ∈ {1, 2, ..., nlist/10}`，对每组做 Brent 求 λ̂，选使平均 probe 数最少的
6. **Brent 求 λ̂**: 在 calib 集上，用选定的 (k_reg, λ_reg)，在 [0, 1] 上求解 `FNR(λ) = α`
7. **评估 (可选)**: 在 test 集上报告实际 FNR 和平均 probe 数

### RAPS 正则化

```
E(q, p) = (1 - nonconf(q, p)) + λ_reg · max(0, p - k_reg)
max_reg_val = 1 + λ_reg · max(0, nlist - k_reg)
reg_score(q, p) = E(q, p) / max_reg_val
```

### Brent 求根

- 区间: [0, 1]
- 目标: f(λ) = FNR(λ) - α = 0
- FNR(λ): 对每条 calib 查询，从 p=1 开始 probe，当 `reg_score(q, p) > λ` 时停在 p，检查此时 top-k 是否包含 GT
- 使用 GSL `gsl_root_fsolver_brent`
- tolerance: 1e-6, 最大迭代: 100

### 约束

- compute_scores 阶段使用精确 L2 距离（不用 RaBitQ 近似）
- 标定结果需要持久化（写入 segment metadata 或单独文件）
- d_min, d_max 从 calib 集全局统计
