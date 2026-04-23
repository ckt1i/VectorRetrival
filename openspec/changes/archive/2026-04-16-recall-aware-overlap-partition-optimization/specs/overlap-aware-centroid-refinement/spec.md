## ADDED Requirements

### Requirement: centroid 必须支持在 overlap membership 下做 refinement
系统 SHALL 支持一种 centroid-refinement stage，在引入 overlap assignment 之后继续更新 coarse centroids，使 centroid 几何结构能够反映 multi-assignment membership。

#### Scenario: 启用 overlap-aware centroid refinement
- **WHEN** 使用 overlap-aware centroid refinement 配置索引构建
- **THEN** build process SHALL 在 overlap assignments 推导完成后至少执行一轮 centroid update

#### Scenario: refinement stage 被写入元数据
- **WHEN** 启用 overlap-aware centroid refinement
- **THEN** 构建元数据 SHALL 明确记录已应用 overlap membership 下的 centroid refinement

### Requirement: Overlap-aware refinement 必须保留可对照的 partition 输出
系统 SHALL 保留足够的 partition 元数据，使 refined 与 non-refined overlap variant 能在同一 coarse-cover 诊断工作流下对照。

#### Scenario: refined partition 可参与 coarse-cover 诊断
- **WHEN** 对 refined overlap partition 执行评估
- **THEN** coarse-cover 诊断输出 SHALL 能识别其 refinement stage 或 variant identity

#### Scenario: non-refined overlap 仍可作为基线保留
- **WHEN** 关闭 overlap-aware centroid refinement
- **THEN** 系统 SHALL 继续支持 non-refined overlap build，作为对照基线
