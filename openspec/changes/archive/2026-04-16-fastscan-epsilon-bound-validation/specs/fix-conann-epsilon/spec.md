## 新增需求

### 需求：运行时 FastScan epsilon 必须来自已加载的索引元数据
运行时查询路径必须使用从 `segment.meta` 中加载出来的 `conann.epsilon` 作为当前 FastScan 的误差边界。复用预构建索引时，查询阶段的 CLI 参数不能覆盖这个值。

#### 场景：预构建索引保留自己的 epsilon
- **当** benchmark 运行通过 `--index-dir` 复用一个已存在的索引时
- **则** 运行时查询路径必须使用该索引 `segment.meta` 中保存的 epsilon
- **并且** 仅在 benchmark CLI 上修改 `--epsilon-percentile`，不能改变运行时 FastScan 边界

### 需求：`epsilon-percentile` 只能通过重建阶段影响 FastScan 边界
系统必须把 `epsilon-percentile` 解释为 FastScan `eps_ip` 的构建阶段输入；只有在用新 percentile 重建索引之后，运行时 Stage 1 边界才允许变化。

#### 场景：重建后 runtime epsilon 发生变化
- **当** 索引使用不同的 `epsilon-percentile` 被重新构建时
- **则** 产出的 `segment.meta.conann.epsilon` 可以变化
- **并且** 后续对这个重建索引的查询运行必须使用新的运行时 FastScan 边界

### 需求：FastScan epsilon 校准必须被记录为当前边界来源
实验和验证输出必须说明，当前 FastScan Stage 1 的边界是由校准后的运行时 `eps_ip` 决定的，而不是由一个独立的查询阶段 epsilon 参数决定的。

#### 场景：验证报告描述边界来源
- **当** 产出一份 epsilon 验证报告时
- **则** 报告必须说明运行时 Stage 1 边界由加载后的 `eps_ip` 决定
- **并且** 报告必须将这个来源与历史上的 popcount epsilon 描述区分开
