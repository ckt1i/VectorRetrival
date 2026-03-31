# Spec: Multi-bit RaBitQ 编码与估算

## Capability

支持 M-bit（M=1,2,4）RaBitQ 量化编码和两阶段距离估算。

## Requirements

### R1: M-bit 编码

- **R1.1**: `RaBitQEncoder` 构造时接受 `bits` 参数（1, 2, 4）
- **R1.2**: 编码输出采用 bit-plane 布局，MSB plane 在前
- **R1.3**: MSB plane 等价于 1-bit sign code（`(v >= 0) ? 1 : 0`）
- **R1.4**: `RaBitQCode.bits` 记录量化位数
- **R1.5**: `RaBitQCode.sum_x` 始终为 MSB plane 的 popcount
- **R1.6**: `bits=1` 时行为与修改前完全一致

### R2: M-bit 距离估算

- **R2.1**: `RaBitQEstimator` 构造时接受 `bits` 参数
- **R2.2**: `PrepareQuery` 当 `bits > 1` 时预计算 LUT[2^M]
- **R2.3**: LUT 公式：`LUT[v] = (-1.0 + (2*v + 1.0) / 2^M) / √L`
- **R2.4**: `EstimateDistance` 始终使用 MSB plane popcount（Stage 1）
- **R2.5**: 新增 `EstimateDistanceMultiBit` 使用完整 M-bit LUT scan（Stage 2）
- **R2.6**: `bits=1` 时 `EstimateDistanceMultiBit` 不应被调用（调用方跳过）

### R3: ε_ip 经验公式

- **R3.1**: ε_ip_s1 通过现有采样校准获得（不变）
- **R3.2**: ε_ip_s2 = ε_ip_s1 / 2^(M-1)
- **R3.3**: margin_s1 = 2 · r_max · r_q · ε_ip_s1
- **R3.4**: margin_s2 = margin_s1 / 2^(M-1)

### R4: 两阶段分类

- **R4.1**: Stage 1 对所有候选执行 MSB popcount 分类
- **R4.2**: Stage 2 仅对 Stage 1 的 Uncertain 执行 M-bit LUT 分类
- **R4.3**: `bits=1` 时跳过 Stage 2
- **R4.4**: SafeIn（S1 或 S2）→ 读原始向量 + payload
- **R4.5**: SafeOut（S1 或 S2）→ 跳过
- **R4.6**: Uncertain（最终）→ 仅读原始向量 → rerank → 确认 top-K 后读 payload

### R5: Benchmark 统计

- **R5.1**: bench_rabitq_accuracy 和 bench_vector_search 支持 `--bits` 参数
- **R5.2**: 分别记录 S1/S2 的 SafeIn、SafeOut、Uncertain 计数
- **R5.3**: 分别记录 S1/S2 的 False SafeIn、False SafeOut 计数和比例
- **R5.4**: False SafeIn = 被分类为 SafeIn 但不在 GT top-K
- **R5.5**: False SafeOut = 被分类为 SafeOut 但在 GT top-K

## Acceptance Criteria

- `bits=1` 所有现有测试通过，行为不变
- `bits=2` 编码后 MSB plane 与 `bits=1` 编码结果一致
- `bits=2` 的 EstimateDistanceMultiBit 误差小于 EstimateDistance (Stage 1)
- bench_rabitq_accuracy 输出包含 S1/S2 双阶段统计
- bench_vector_search 输出包含 False SafeIn/Out 比例
