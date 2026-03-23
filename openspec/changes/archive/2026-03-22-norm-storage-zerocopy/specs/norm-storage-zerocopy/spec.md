# Spec: Norm Storage & Zero-Copy ProbeCluster

## Requirements

### R1: Code Entry 格式 (AoS + 8B Aligned)

每条 RaBitQ code entry 由三部分组成，连续存储:
1. `code_bits`: `uint64_t[num_words]` — 原始 binary code
2. `norm_oc`: `float32` — ‖o - c‖₂
3. `sum_x`: `uint32_t` — popcount(code)

`code_entry_size = num_words * 8 + 8` 字节，保证 8 字节对齐。

### R2: .clu 文件版本升级

Global header version 字段从 4 升到 5。Reader 必须根据 version 选择正确的 `code_entry_size` 计算方式:
- v4: `num_words * 8`
- v5: `num_words * 8 + 8`

### R3: 零拷贝 ProbeCluster

ProbeCluster 不得为每个向量构造 `RaBitQCode` 对象或分配 heap 内存。必须直接从 `ParsedCluster.block_buf` 中通过指针算术读取 code/norm/sum_x。

### R4: EstimateDistanceRaw 接口

`RaBitQEstimator` 提供零拷贝接口:
```
float EstimateDistanceRaw(pq, code_words_ptr, num_words, norm_oc, sum_x)
```
行为与 `EstimateDistance(pq, RaBitQCode)` 完全一致。

### R5: RaBitQ 精度 Benchmark

在 coco_1k 数据集上:
- 对每个 query，计算所有 1000 个向量的 exact L2² 距离和 RaBitQ 估计距离
- 报告: 距离绝对误差 (mean/max), 相对误差 (mean/p95/p99)
- 报告: recall@1, recall@5, recall@10 (RaBitQ brute-force vs exact brute-force)
- 报告: SafeIn/SafeOut 分类准确率 (以 exact top-K 为 ground truth)
- 特别报告: False SafeOut rate (实际 top-K 但被分类为 SafeOut — 正确性风险)

## Constraints

- 不修改 `RaBitQCode` struct 定义 (保留向后兼容性)
- benchmark 不依赖新的 .clu 格式 (可直接在内存中编码和估计)
- benchmark 输出纯文本到 stdout，不写文件
