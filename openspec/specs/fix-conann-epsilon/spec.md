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

## Additional Requirements (from fastscan-epsilon-bound-validation)

### 需求：运行时 FastScan epsilon 必须来自已加载的索引元数据
运行时查询路径必须使用从 `segment.meta` 中加载出来的 `conann.epsilon` 作为当前 FastScan 的误差边界。复用预构建索引时，查询阶段的 CLI 参数不能覆盖这个值。

#### Scenario: 预构建索引保留自己的 epsilon
- **当** benchmark 运行通过 `--index-dir` 复用一个已存在的索引时
- **则** 运行时查询路径必须使用该索引 `segment.meta` 中保存的 epsilon
- **并且** 仅在 benchmark CLI 上修改 `--epsilon-percentile`，不能改变运行时 FastScan 边界

### 需求：`epsilon-percentile` 只能通过重建阶段影响 FastScan 边界
系统必须把 `epsilon-percentile` 解释为 FastScan `eps_ip` 的构建阶段输入；只有在用新 percentile 重建索引之后，运行时 Stage 1 边界才允许变化。

#### Scenario: 重建后 runtime epsilon 发生变化
- **当** 索引使用不同的 `epsilon-percentile` 被重新构建时
- **则** 产出的 `segment.meta.conann.epsilon` 可以变化
- **并且** 后续对这个重建索引的查询运行必须使用新的运行时 FastScan 边界

### 需求：FastScan epsilon 校准必须被记录为当前边界来源
实验和验证输出必须说明，当前 FastScan Stage 1 的边界是由校准后的运行时 `eps_ip` 决定的，而不是由一个独立的查询阶段 epsilon 参数决定的。

#### Scenario: 验证报告描述边界来源
- **当** 产出一份 epsilon 验证报告时
- **则** 报告必须说明运行时 Stage 1 边界由加载后的 `eps_ip` 决定
- **并且** 报告必须将这个来源与历史上的 popcount epsilon 描述区分开

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
