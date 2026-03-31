# Proposal: Multi-bit RaBitQ 量化与两阶段查询

## Problem

当前系统仅支持 1-bit RaBitQ 量化（UNDO.txt [PHASE4-002]）。1-bit 量化压缩率极高（32×），但距离估算误差较大，导致：

1. **ε_ip 较大** → ConANN margin 宽 → Uncertain 区间大 → 更多向量需要 rerank I/O
2. **Recall 上限受限**：1-bit popcount 估算的内积精度不足以在高精度场景下使用
3. **bench_vector_search 缺少 False SafeIn/Out 统计**，无法评估分类正确性

## Proposed Change

### 1. Multi-bit RaBitQ 编码（M=1,2,4）

扩展 RaBitQEncoder/Estimator 支持 M-bit 量化，通过 `RaBitQConfig.bits` 运行时控制：

- **编码**：将旋转后的归一化分量均匀量化为 2^M 个 bin，采用 bit-plane 布局存储（MSB plane 在前）
- **MSB plane = 1-bit sign code**：与现有 1-bit 完全兼容，Stage 1 零额外开销

### 2. 两阶段查询流水线

```
所有候选 ──→ Stage 1 (MSB popcount) ──┬→ SafeIn_S1  → 读 vec + payload
                                       ├→ SafeOut_S1 → 丢弃
                                       └→ Uncertain_S1
                                              │
                              (仅当 bits > 1) │
                                              ▼
                                  Stage 2 (M-bit LUT) ──┬→ SafeIn_S2  → 读 vec + payload
                                                        ├→ SafeOut_S2 → 丢弃
                                                        └→ Uncertain_S2 → 仅读 vec
                                                                            │
                                                                    Exact Rerank
                                                                            │
                                                                  确认 top-K → 读 payload
```

I/O 语义：
- **SafeIn**（S1/S2）：确信在 top-K → 读完整原始向量 + payload
- **SafeOut**（S1/S2）：确信不在 top-K → 跳过，节省 I/O
- **Uncertain_S2**：不确定 → 仅读原始向量做 exact rerank → rerank 确认后再读 payload

### 3. ε_ip 经验公式简化

仅校准 1-bit 的 ε_ip_s1，Stage 2 的误差自动导出：

```
ε_ip_s2 = ε_ip_s1 / 2^(M-1)
margin_s2 = margin_s1 / 2^(M-1)
```

M=1 时退化为单 stage（ε_ip_s2 = ε_ip_s1，Stage 2 自动跳过）。

### 4. Benchmark 增强

- **bench_rabitq_accuracy**：对比 M=1,2,4 下的距离误差、recall、S1/S2 分类统计及 False SafeIn/Out
- **bench_vector_search**：新增 False SafeIn/Out 比例统计（对照 GT）

## Non-Goals

- AVX-512 SIMD 优化（保持 UNDO.txt 延迟状态）
- M=8 支持（压缩率太低，优先级低）
- 生产 query pipeline（overlap_scheduler）集成（先在 benchmark 中验证）
- 存储层（.clu 文件格式）变更（benchmark 使用内存编码，不落盘）

## Success Criteria

- `bits=1` 行为与修改前完全一致（回归测试通过）
- `bits=2` 在 bench_rabitq_accuracy 上 recall@10 优于 `bits=1`
- bench_vector_search 输出包含 False SafeIn/Out 比例
- S1/S2 各阶段统计清晰展示
