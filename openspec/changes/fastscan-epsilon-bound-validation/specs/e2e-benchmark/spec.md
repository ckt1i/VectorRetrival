## 新增需求

### 需求：E2E benchmark 必须报告 epsilon 验证元数据
warm-serving benchmark 的输出必须包含足够的元数据，用来区分构建阶段的 epsilon 输入和运行时加载到的 FastScan 边界。

#### 场景：输出中可见加载后的 epsilon
- **当** `bench_e2e` 在重建索引或预构建索引上运行时
- **则** 输出中必须包含加载后的运行时 `eps_ip`
- **并且** 输出中必须标明这次运行使用的是重建后的索引，还是通过 `--index-dir` 复用的索引

### 需求：E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证的 benchmark 输出必须包含分析候选流变化所需的 Stage 1 和 Stage 2 计数，而不能只输出最终延迟。

#### 场景：候选流统计被导出
- **当** 一次 epsilon 验证运行结束时
- **则** 输出中必须包含 `avg_total_probed`
- **并且** 输出中必须包含 Stage 1 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `Uncertain`
- **并且** 输出中必须包含 `avg_probe_time_ms`
- **并且** 输出中必须包含 `avg_rerank_cpu_ms`

### 需求：E2E benchmark 必须支持固定点 epsilon 验证运行
benchmark 协议必须支持在多个重建索引之间重复同一个 warm-serving 服务点，从而只让 epsilon 标定发生变化。

#### 场景：固定点 warm 对比
- **当** 多个 epsilon 实验点被执行时
- **则** benchmark 必须保持数据集、查询数、`nprobe`、`crc-alpha` 和 `clu_mode` 不变
- **并且** 对比表中只能变化重建后的索引身份及其加载到的 `eps_ip`
