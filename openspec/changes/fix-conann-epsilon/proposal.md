# Proposal: Fix ConANN Epsilon — Inner-Product Error Bound

## Problem

当前 ConANN 的 epsilon 基于向量重建误差 `L2(v, v̂)`（P95），这与 RaBitQ 距离估计的误差机制不匹配：

1. **距离空间混淆**：epsilon 是 L2 空间的重建误差（≈0.45），而 d_k 和 approx_dist 都是 L2² 空间（≈0.82）。`tau_out = d_k + 2ε` 混用了两个不同空间的量。
2. **误差界形式错误**：RaBitQ 距离估计误差 `|d̂² - d²|` 不是常数，而是 `2·r_i·r_q·ε_ip`，其中 `ε_ip = |ŝ - s|` 是内积估计误差，`r_i = ‖o-c‖`，`r_q = ‖q-c‖`。
3. **后果**：epsilon ≈ 0.45 vs d_k ≈ 0.82，导致 `tau_in < 0`（SafeIn 永远不触发），`tau_out` 过大（几乎无 SafeOut），三分类退化为 100% Uncertain。

## Solution

将 epsilon 的语义从"向量重建误差"改为"内积估计误差上界 ε_ip"：

- **Build 时**：在全量数据集上标定 `ε_ip = P95(|ŝ - s|)`，其中 ŝ 是 popcount 路径的内积估计，s 是精确内积。ε_ip 存入 `segment_meta.ConANNParams.epsilon`。
- **Build 时**：对每个 cluster 计算 `r_max = max(‖o-c‖)` 即残差范数最大值，复用 `.clu` lookup entry 的 `epsilon` 字段存储。
- **Query 时**：ProbeCluster 预计算 per-(cluster, query) 的 margin：`margin = 2 · r_max · r_q · ε_ip`，然后用 `d̂² > d_k + margin` 做 SafeOut 判定。

## Scope

- 修改 `IvfBuilder`: 替换 `ComputeClusterEpsilon` 为 `CalibrateEpsilonIp`（全局 ε_ip）和 per-cluster `r_max` 计算
- 修改 `ConANN` 类：简化为只存 ε_ip 和 d_k（移除 tau_in/tau_out 预计算）
- 修改 `OverlapScheduler::ProbeCluster`: query 时动态计算 margin
- 修改 `.clu` lookup entry epsilon 字段语义：从重建误差 P95 → r_max
- 更新 `segment_meta.fbs` 注释
- 更新 benchmark 和测试
- .clu 文件格式版本不变（v6），字段语义变更

## Non-Goals

- Per-vector adaptive margin（`2·r_i·r_q·ε_ip`）：用 r_max bound r_i，保持内循环零额外计算
- 修改 d_k 标定逻辑
- 修改 RaBitQ 编码/解码逻辑
