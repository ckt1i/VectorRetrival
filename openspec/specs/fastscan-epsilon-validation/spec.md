# Spec: FastScan Epsilon Validation

## Overview

验证 FastScan epsilon bound 与构建阶段 epsilon-percentile 的关系，确认运行时 epsilon 的真实来源，并建立 epsilon 调优的决策框架。

## Requirements

### Requirement: FastScan epsilon 验证必须使用重建阶段的校准点
验证流程必须把 `epsilon-percentile` 视为构建阶段的校准变量。每一个 epsilon 实验点都必须先把索引重建到独立目录，再进行查询 benchmark。

#### Scenario: epsilon 实验必须重建索引
- **当** 一次实验要比较两个不同的 `epsilon-percentile` 值时
- **则** 每个值都必须使用单独重建的索引目录
- **并且** 比较过程中不能让两个实验点复用同一个预构建 `--index-dir`

### Requirement: 验证必须确认运行时 epsilon 的真实来源
每一次 epsilon 验证都必须确认查询时实际加载的 `eps_ip` 是什么，并把这个运行时值视为事实来源。

#### Scenario: 从加载后的索引元数据中记录 epsilon
- **当** `bench_e2e` 打开一个重建后的或预构建的索引时
- **则** 这次运行必须记录查询路径实际使用的 `conann.epsilon` 值
- **并且** 分析表必须把这个加载后的值与构建时的 `epsilon-percentile` 一起报告

### Requirement: 主实验必须隔离运行时服务变量
第一轮验证必须保持 warm-serving 服务点不变，只改变构建阶段的 epsilon 校准点。

#### Scenario: 固定服务点
- **当** 执行主 epsilon 验证时
- **则** 必须使用 `coco_100k`、`queries=1000`、`topk=10`、`nlist=2048`、`nprobe=200`、`crc-alpha=0.02` 和 `clu_mode=full_preload`
- **并且** 在所有 epsilon 点之间，不允许改变任何其他服务侧参数

### Requirement: 在进入 CPU 路径优化前，必须先扫一个最小 epsilon 网格
实验计划必须包含一个足够区分保守边界和更紧边界的第一轮 epsilon 网格，然后再决定是否进入新的 CPU 优化工作。

#### Scenario: 最小 epsilon 网格
- **当** 第一轮验证被安排执行时
- **则** 必须至少包含 `epsilon-percentile={0.90, 0.95, 0.99}`
- **并且** 除非第一轮结果不明确，否则必须固定 `epsilon-samples`

### Requirement: 验证必须定义结果门槛
验证计划必须定义清晰的决策门槛，用来判断下一步是继续 epsilon 调优还是转向 CPU kernel 优化。

#### Scenario: 优先 epsilon 调优
- **当** 降低 `epsilon-percentile` 能明显减少 Stage 2 压力，并且没有明显召回退化时
- **则** 计划进入 epsilon 调优和刷新后的 Pareto 验证

#### Scenario: 优先 CPU 路径优化
- **当** 降低 `epsilon-percentile` 导致召回退化，或者没有明显减少 Stage 2 压力时
- **则** 计划停止 epsilon 调优，转向 CPU 路径优化工作
