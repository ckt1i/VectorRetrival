# Dynamic SafeOut

## Requirements

### R1: ClassifyAdaptive 方法
ConANN 类新增 `ClassifyAdaptive(approx_dist, margin, dynamic_d_k)` 方法：
- SafeOut: `approx_dist > dynamic_d_k + 2 * margin`
- SafeIn: `approx_dist < d_k_ - 2 * margin`（静态 d_k，构造时设定）
- Uncertain: 其余情况

### R2: dynamic_d_k 来源
- 来自 OverlapScheduler 的 est_heap_（RaBitQ estimate max-heap）
- 当 est_heap_.size() >= top_k 时：dynamic_d_k = est_heap_.front().first
- 当 est_heap_ 未满时：fallback 到 static d_k（`index_.conann().d_k()`）

### R3: 更新粒度
- dynamic_d_k 仅在 cluster 之间更新（进入 ProbeCluster 时读取一次）
- cluster 内的多个 batch 间不重新计算 dynamic_d_k
- est_heap_ 本身在每个 vector 处理后持续更新

### R4: 向后兼容
- 现有 `Classify(approx_dist)` 和 `Classify(approx_dist, margin)` 接口保持不变
- 当 crc_params == nullptr 时，ProbeCluster 使用原有的 `Classify(dist, margin)`

## Acceptance Criteria

- [ ] ClassifyAdaptive 在 dynamic_d_k < static d_k 时产生更多 SafeOut
- [ ] SafeIn 行为不受 dynamic_d_k 影响（始终使用静态 d_k）
- [ ] est_heap_ 未满时 fallback 到 static d_k 正常工作
- [ ] 单元测试验证 ClassifyAdaptive 的三分类正确性
