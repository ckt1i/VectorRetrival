# Proposal: Norm Storage in Cluster Block + Zero-Copy ProbeCluster

## What

将 `‖o-c‖` (norm) 和 `sum_x` (popcount) 嵌入到 .clu 文件的 RaBitQ code entry 中，采用 AoS 布局并保持 8 字节对齐。同时重构 ProbeCluster 为零拷贝模式，消除逐向量的 heap 分配。最后新增 RaBitQ 精度 benchmark，在 coco_1k 数据集上测量距离估计准确率和暴力搜索 recall。

## Why

- **norm 缺失**: 当前 code entry 仅存储 binary code，norm 在写入 .clu 后丢失，ProbeCluster 中 `batch_codes[i].norm = 0.0f` 导致距离估计公式 `‖o-c‖² + ‖q-c‖² - 2·‖o-c‖·‖q-c‖·⟨q̄,ô⟩` 完全失效（norm_oc=0 使得 dist ≈ ‖q-c‖² 而非真实距离）
- **性能浪费**: `code.assign(words, words+num_words_)` 每个向量做一次 heap alloc + memcpy，batch_size=64 时每个 cluster 有 O(N) 次分配
- **缺乏精度基线**: 没有量化衡量 1-bit RaBitQ 估计距离与真实 L2 距离的偏差

## Scope

### In Scope
- .clu 文件格式 v5: code entry 扩展为 `[code_bits | norm(f32) | sum_x(u32)]`
- ClusterStoreWriter::WriteVectors 写入 norm + sum_x
- ClusterStoreReader::ParseBlock 解析新格式
- ProbeCluster 零拷贝: 直接从 block_buf 读取 code/norm/sum_x，不做 std::vector 分配
- RaBitQEstimator 新增零拷贝接口 (接受 raw pointer 而非 RaBitQCode)
- RaBitQ 精度 benchmark (bench_rabitq_accuracy.cpp)

### Out of Scope
- SoA 布局转换
- 多 bit (2/4/8-bit) RaBitQ 支持
- SIMD 批量距离公式 (后续优化)
- c_factor 自适应标定 (后续 change)

## Decisions

- **AoS + 8 字节对齐**: `code_entry_size = num_words * 8 + 8`，norm 和 sum_x 紧随 code bits，保证下一条 entry 起始地址 8B 对齐
- **文件版本号 4→5**: 向量存储格式变化是 breaking change
- **零拷贝**: ProbeCluster 不再构造 `RaBitQCode` 对象，直接用 `const uint64_t*` + `float norm` + `uint32_t sum_x` 调用估计器
