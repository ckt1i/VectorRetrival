# RaBitQ CRC Calibration

## Requirements

### R1: CrcCalibrator 使用 RaBitQ 估计距离
- `ComputeScoresForQuery` 中的距离计算从 `simd::L2Sqr` 改为 `RaBitQEstimator::EstimateDistanceRaw`
- 每个 cluster 需要提供 RaBitQ codes block（1-bit 量化码 + norm_oc）
- 需要 rotation matrix 用于 PrepareQuery

### R2: ClusterData 扩展
```cpp
struct ClusterData {
    const float* vectors;         // 原始向量（保留，可能用于 GT）
    const uint32_t* ids;
    uint32_t count;
    const uint8_t* codes_block;   // RaBitQ encoded block
    uint32_t code_entry_size;      // bytes per entry (code_words * 8 + 4)
};
```

### R3: Calibrate 接口扩展
新增参数：
- `const RotationMatrix& rotation`: RaBitQ rotation matrix
- ClusterData 中的 codes_block 和 code_entry_size

### R4: 距离空间一致性
- 标定输出的 d_min/d_max 是 RaBitQ 估计距离空间的统计量
- 线上 CrcStopper.ShouldStop 输入的 current_kth_dist 也是 RaBitQ 估计距离
- 两者在同一距离空间内，conformal guarantee 成立

### R5: Ground truth 仍使用精确距离
- ground_truth 数组仍由精确 L2 计算得到（外部传入）
- CRC 标定评估 FNR 时，用 RaBitQ-based predictions 对比 exact-L2 ground truth
- 这模拟了真实场景：线上用 RaBitQ 做决策，但关心的是精确距离下的 recall

## Acceptance Criteria

- [ ] CrcCalibrator 使用 RaBitQ 距离时能正常完成标定（lamhat 在合理范围）
- [ ] 标定结果的 d_min/d_max 反映 RaBitQ 距离空间
- [ ] test set 上的 FNR 在 alpha 附近（可能略高于 exact L2 标定，因为 RaBitQ 有误差）
- [ ] 标定后的 CrcStopper 在 OverlapScheduler 中正常工作
