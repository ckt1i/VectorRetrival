# Spec: Fix ConANN Epsilon — Inner-Product Error Bound

## Requirements

### R1: ε_ip 全局标定
- Build 时计算内积估计误差: `|ŝ - s|`，其中 ŝ 是 popcount 路径内积估计，s 是精确内积
- Cluster 内向量互作 pseudo-query 进行标定
- 采样策略: 每 cluster 采 `min(epsilon_samples, n_members)` 个 query，与同 cluster 全量配对
- `ε_ip = P95(全局 |ŝ - s| pool)`
- 存入 `segment_meta.ConANNParams.epsilon`

### R2: Per-Cluster r_max
- 每个 cluster 计算 `r_max = max(code.norm for code in cluster)`
- 复用 `.clu` lookup entry 的 `float epsilon` 字段存储 r_max
- .clu 文件版本保持 v6 不变

### R3: Query 时动态 Margin
- ProbeCluster 预计算: `margin = 2 · r_max · pq.norm_qc · ε_ip`
- SafeOut: `d̂² > d_k + margin`
- SafeIn:  `d̂² < d_k - margin`
- 内循环不增加 per-vector 计算

### R4: ConANN 接口更新
- 新增 `Classify(float approx_dist, float margin)` 重载
- 旧 `Classify(float approx_dist)` 保留向后兼容（使用存储的 epsilon 作为 margin）
- ConANN 仍存储 ε_ip 和 d_k

### R5: Benchmark 更新
- `bench_rabitq_accuracy` 输出 ε_ip 值和 per-cluster r_max 统计
- 分类统计使用正确的 per-(cluster, query) margin

## Constraints

### C1: Build-Time Only
- ε_ip 和 r_max 标定只在 build 阶段进行
- Query 时只做乘法预计算 margin

### C2: 向后不兼容
- v6 文件 epsilon 字段语义从"重建误差 P95"变为 r_max
- 需要重新 build 索引

### C3: P95 Percentile
- ε_ip 使用 P95 分位数
- 与 per-cluster epsilon 方案保持一致的保守度

## Acceptance Criteria

- [ ] ε_ip 标定值对 dim=768 应在 O(1/√dim) ≈ 0.036 量级
- [ ] cluster_store_test: r_max 写入后读回值一致
- [ ] overlap_scheduler_test: 使用正确 margin，结果与 brute-force 一致
- [ ] bench_rabitq_accuracy: SafeOut 比例显著提升（> 50% for typical datasets）
- [ ] 编译无 warning，所有现有 test 通过
