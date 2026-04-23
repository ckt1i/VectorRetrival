## MODIFIED Requirements

### Requirement: E2E benchmark 必须报告 epsilon 验证元数据
warm-serving benchmark 的输出必须包含足够的元数据，用来区分构建阶段的 epsilon 输入和运行时加载到的 FastScan 边界。

#### Scenario: 输出中可见加载后的 epsilon
- **当** `bench_e2e` 在重建索引或预构建索引上运行时
- **则** 输出中必须包含加载后的运行时 `eps_ip`
- **并且** 输出中必须标明这次运行使用的是重建后的索引，还是通过 `--index-dir` 复用的索引

### Requirement: E2E benchmark MUST preserve prepare and Stage1 fine-grained observability for fused-kernel comparison
在 query 主路径继续推进 fused `quantize + lut_build` 和 Stage1 后续 SIMD 优化时，benchmark 输出 SHALL 保持 prepare 与 Stage1 的细粒度子项可见，以便同口径比较参考路径、保留两段版和融合版。

prepare 至少 MUST 包含：

- `probe_prepare_subtract_ms`
- `probe_prepare_normalize_ms`
- `probe_prepare_quantize_ms`
- `probe_prepare_lut_build_ms`

Stage1 至少 MUST 包含：

- `probe_stage1_estimate_ms`
- `probe_stage1_mask_ms`
- `probe_stage1_iterate_ms`
- `probe_stage1_classify_only_ms`
- `probe_submit_ms`

#### Scenario: Fused prepare is benchmarked under the same field schema
- **WHEN** 系统切换到 fused `quantize + lut_build` 路径
- **THEN** benchmark 输出 MUST 仍然保留 prepare 四段字段
- **AND** 这些字段 MUST 能与参考路径直接对比

#### Scenario: Stage1 follow-up SIMD work remains attributable
- **WHEN** survivor compaction、batch classify 或 submit-prep 被继续优化
- **THEN** benchmark 输出 MUST 仍然能区分 Stage1 子项和 submit CPU 成本
- **AND** 不得把 submit 开销重新混入 Stage1 内部字段
