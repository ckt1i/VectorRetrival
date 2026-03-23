# Design: Fix ConANN Epsilon

## Mathematical Foundation

### RaBitQ 距离估计公式

```
d̂²_i = r²_i + r²_q - 2·r_i·r_q·ŝ_i

其中:
  r_i  = ‖o_i - c‖     (候选残差范数，存储在 .clu code entry)
  r_q  = ‖q - c‖       (query 残差范数，PrepareQuery 计算)
  ŝ_i  = ⟨q̄, ô_i⟩_est  (内积估计，来自 popcount)
  s_i  = ⟨q̄, ô_i⟩      (真实内积)
```

### 误差传播

```
|d̂² - d²| = |2·r_i·r_q·(ŝ_i - s_i)|
           ≤ 2·r_i·r_q·|ŝ_i - s_i|
           ≤ 2·r_i·r_q·ε_ip

对 cluster 内取上界:
  r_i ≤ r_max^(c)

per-(cluster, query) margin:
  margin^(c,q) = 2·r_max^(c)·r_q·ε_ip
```

### ConANN 判定

```
SafeOut:   d̂²_i > d_k + margin^(c,q)
SafeIn:    d̂²_i < d_k - margin^(c,q)
Uncertain: otherwise
```

其中 d_k 是精确 L2² 标定的阈值，没有 RaBitQ 误差。

## ε_ip 标定

### 方法

Build 时对全量数据集（或采样）计算内积误差：

1. 对每个 cluster c 中的向量 i，选择同 cluster 的向量 j 作为 pseudo-query
2. 计算精确内积: `s = ⟨q̄_j, ô_i⟩`（float dot product of rotated normalized residuals）
3. 计算估计内积: `ŝ = 1 - 2·hamming(sign_j, code_i)/dim`
4. 收集 `|ŝ - s|` 到全局 pool
5. `ε_ip = Percentile(pool, P95)`

### 实现路径

在 IvfBuilder 中，编码完所有 cluster 后，遍历 cluster 做标定：
- Build 阶段已有 RotationMatrix、所有 RaBitQ codes、归一化残差
- 可以同时计算 r_max（遍历 codes 取 max norm）

### 采样策略

- 每个 cluster 采样 `min(n_samples, n_members)` 个 pseudo-query
- 每个 pseudo-query 与同 cluster 全部向量配对计算
- n_samples 由 `IvfBuilderConfig.epsilon_samples` 控制

## 存储设计

### segment_meta.ConANNParams

```
epsilon: float;  // ε_ip: 全局内积估计误差上界 P95
d_k: float;      // 不变
```

### .clu Lookup Table Entry

```
float epsilon;   // 语义变更: r_max^(c) = max(‖o-c‖) within cluster
```

格式版本保持 v6 不变（字段大小、位置不变，仅语义变更）。

## Query 路径

```
ProbeCluster(cluster_id):
  pq = PrepareQuery(query, centroid, rotation)  // pq.norm_qc = r_q
  r_max = pc.epsilon                             // 从 lookup entry 读取
  eps_ip = index_.conann().epsilon()              // 全局 ε_ip
  d_k = index_.conann().d_k()

  margin = 2.0f * r_max * pq.norm_qc * eps_ip
  tau_out = d_k + margin
  tau_in  = d_k - margin

  for each vector i:
    dist = EstimateDistanceRaw(pq, ...)
    if dist > tau_out → SafeOut
    if dist < tau_in  → SafeIn
    else → Uncertain
```

内循环不变，只在 cluster 粒度预计算一次 margin。

## ConANN 类简化

ConANN 不再预计算 tau_in/tau_out（因为 margin 依赖 query 时的 r_q）。保留 Classify 接口，但改为接受 margin 参数

选择 **选项 B**，保持接口一致性：

```cpp
ResultClass Classify(float approx_dist, float margin) const {
    if (approx_dist > d_k_ + margin) return SafeOut;
    if (approx_dist < d_k_ - margin) return SafeIn;
    return Uncertain;
}
```

旧的 `Classify(float approx_dist)` 保留但使用内部存储的 epsilon 作为 margin（向后兼容 benchmark）。

## 决策记录

| 决策 | 选项 | 理由 |
|------|------|------|
| r_max 存储位置 | 复用 .clu epsilon 字段（选项 A）| 避免 v7 版本升级，字段大小不变 |
| margin 粒度 | per-(cluster, query)，用 r_max bound r_i | 内循环零额外开销 |
| ε_ip 标定范围 | 全局（跨所有 cluster） | 一个标量即可，无需 per-cluster |
| ConANN 接口 | 新增 Classify(dist, margin) 重载 | 保持向后兼容 |
