# Spec: Recall-Aware Secondary Assignment

## Overview

Recall-oriented secondary assignment strategy that uses complementary evidence to select overlap clusters.

## Requirements

### Requirement: Secondary assignment 必须支持 recall-aware 的互补选择
系统 SHALL 支持一种 secondary assignment 策略，该策略使用 recall-oriented 的互补证据来选择 overlap cluster，而不是仅依赖几何 second-nearest 或 residual-aware 启发式。

#### Scenario: secondary assignment 使用 recall-aware 策略
- **WHEN** 使用 recall-aware secondary assignment policy 配置索引构建
- **THEN** 所有启用了 overlap 的向量 SHALL 通过 recall-oriented 的互补选择规则获得其 second cluster

#### Scenario: assignment policy identity 被写入元数据
- **WHEN** 使用 recall-aware secondary assignment policy 构建索引
- **THEN** 构建元数据 SHALL 记录该 secondary assignment policy identity，以便与现有 overlap variant 对照

### Requirement: Recall-aware secondary assignment 必须可与现有 overlap mode 比较
系统 SHALL 保留现有 overlap policy，并允许 recall-aware secondary assignment 在同一 coarse-cover 诊断工作流下与其对比。

#### Scenario: 现有 overlap mode 仍然可用
- **WHEN** 用户配置 `single`、`redundant_top2_naive` 或 `redundant_top2_rair`
- **THEN** build system SHALL 继续支持这些模式，并与新的 recall-aware policy 并存

#### Scenario: recall-aware 与 residual-aware 策略可对照
- **WHEN** 跨多个 overlap policy 运行诊断实验
- **THEN** 输出 SHALL 能区分 recall-aware policy 与 residual-aware、naive policy 的结果
