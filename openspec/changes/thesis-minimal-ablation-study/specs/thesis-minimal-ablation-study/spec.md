## ADDED Requirements

### Requirement: Thesis ablation study SHALL inherit main-sweep execution controls
论文消融研究必须继承 `thesis-minimal-main-sweep` 的核心实验控制，确保后续机制实验与主实验结果可比较。

#### Scenario: 基础运行参数继承主实验
- **WHEN** 调度任意 ablation、result hygiene 或 top-k supplement 运行
- **THEN** 数据集 SHALL 仅限 `coco_100k` 与 `msmarco_passage`
- **AND** 查询数 SHALL 使用 `queries=1000`
- **AND** BoundFetch 默认主线参数 SHALL 保持 `crc=1`、`early-stop=0`、`bits=4`
- **AND** 正式 measurement SHALL 使用 `skip_gt=0`

#### Scenario: canonical artifact 复用约束继承主实验
- **WHEN** 调度任意消融或 top-k 补充运行
- **THEN** 运行 SHALL 复用对应数据集已发布的 canonical artifact
- **AND** 运行 SHALL 记录 canonical artifact 目录、builder identity、nlist、nprobe 与来源 run id
- **AND** 运行 SHALL NOT 重新从 raw vectors 训练 coarse clustering、生成 centroid 或生成 assignment

#### Scenario: warmup 与 measurement 必须分离
- **WHEN** 记录任意正式消融或 top-k 补充结果
- **THEN** warmup 输出 SHALL 与正式 measurement 输出分离
- **AND** warmup 输出 SHALL NOT 进入论文图表或 ablation summary 的正式数值
- **AND** measurement 输出 SHALL 保留 recall、latency、canonical artifact、`crc`、`early-stop`、`skip_gt` 与 warmup provenance

#### Scenario: BoundFetch 正式 measurement 前必须预热
- **WHEN** 调度任意 BoundFetch result hygiene、triage ablation、scheduling ablation 或 top-k supplement 正式 measurement
- **THEN** runner SHALL 先执行一次同数据集、同 canonical artifact、同 nlist、同 nprobe、同 topk、同 `crc`、同 `early-stop`、同 `bits`、同 `skip_gt`、同调度/triage 变体参数的 BoundFetch warmup run
- **AND** 正式 measurement SHALL 在该 warmup run 完成后执行
- **AND** tracker SHALL 保留 warmup run id 与 measurement run id 的关联
- **AND** ablation summary 与论文图表 SHALL 只读取 warmup 后的 measurement run
- **AND** 若缺少对应 warmup 或 warmup 失败，该 BoundFetch measurement SHALL NOT 被纳入正式消融证据

#### Scenario: 非主线参数只作为消融或调试证据
- **WHEN** BoundFetch 运行使用不同于 `crc=1, early-stop=0, bits=4` 的参数组合
- **THEN** tracker SHALL 将其标记为 ablation、debug 或 tuning
- **AND** 该运行 SHALL NOT 替代 BoundFetch-Guarded 主线点
- **AND** 该运行只有在本 change 明确需要时才能进入消融分析

### Requirement: Thesis ablation study SHALL start from frozen mainline operating points
论文消融研究须以已完成的 `topk=10` 主实验为基准，并使用固定的后续实验运行点进行对比。

#### Scenario: 非 top-k 消融运行点在执行前冻结
- **WHEN** 发起消融任务
- **THEN** COCO 非 top-k 消融 SHALL 使用 `nprobe=64`
- **AND** MS MARCO 非 top-k 消融 SHALL 使用 `nprobe=128`
- **AND** 每个代表点需记录数据集、系统、存储后端、canonical artifact、nlist、nprobe、topk、recall、latency 与来源 run id
- **AND** 所有消融比较必须基于该固定点，而非临时调参点

#### Scenario: 异常主 sweep 点在尾延迟结论前处理
- **WHEN** 某主实验点被判定为尾延迟异常
- **THEN** 在形成 p95/p99 结论前先重跑或明确标注为异常
- **AND** 消融汇报必须说明该点是保留、替换还是剔除

### Requirement: Thesis ablation study SHALL isolate BoundFetch triage decisions
消融研究须通过固定运行点的变体比较，隔离 SafeIn、SafeOut、Uncertain 的贡献。

#### Scenario: SafeOut 剪枝关闭对比
- **WHEN** 运行 SafeOut 剪枝关闭变体
- **THEN** 与对应主线 BoundFetch 保持同一数据集、canonical artifact、nlist、nprobe、topk、warm-serving 协议与查询数
- **AND** 输出 recall、平均时延、p95、p99、rerank 候选数、读字节、SafeIn/SafeOut/Uncertain 比例
- **AND** 该运行不进入主实验 main summary

#### Scenario: SafeIn payload 预取关闭对比
- **WHEN** 运行 SafeIn payload 预取关闭变体
- **THEN** 与对应主线 BoundFetch 保持同一数据集、canonical artifact、nlist、nprobe、topk、warm-serving 协议与查询数
- **AND** 输出 recall、平均时延、p95、p99、payload 预取读、缺失 payload 读、读字节、SafeIn/SafeOut/Uncertain 比例
- **AND** 该运行不进入主实验 main summary

#### Scenario: Uncertain eager payload 对比
- **WHEN** 运行 Uncertain eager payload 变体
- **THEN** 与对应主线 BoundFetch 保持同一数据集、canonical artifact、nlist、nprobe、topk、warm-serving 协议与查询数
- **AND** 输出 recall、平均时延、p95、p99、读字节、payload 读、rerank 候选数、SafeIn/SafeOut/Uncertain 比例
- **AND** 该运行不进入主实验 main summary

#### Scenario: triage 比率作为结果分析参考
- **WHEN** 运行任意 triage 消融或完整策略对照
- **THEN** 输出 SHALL 包含 SafeIn/SafeOut/Uncertain 三类比例
- **AND** 该比例 SHALL 与 rerank 数、payload 读、读字节和尾延迟一起进入结果分析

#### Scenario: triage 消融允许最小实验补丁
- **WHEN** 当前 runner 缺少 SafeOut 关闭、SafeIn prefetch 关闭或 Uncertain eager payload 的必要控制
- **THEN** 可以添加最小实现补丁来暴露实验 flag 或输出 triage 比率
- **AND** 补丁 SHALL 不改变 BoundFetch 主线默认行为
- **AND** 补丁 SHALL 只服务于消融控制与结果记录

### Requirement: Thesis ablation study SHALL isolate dynamic scheduling behavior
消融研究须固定候选点比较 BoundFetch 的调度策略变化对平均时延和尾延迟的影响。

#### Scenario: submit-online 参数对比
- **WHEN** 运行 submit-online 消融
- **THEN** 比较 `submit-online=0` 与 `submit-online=1`
- **AND** 在 COCO `nprobe=64` 的同一 canonical artifact、nlist、topk、查询数和 warm-serving 协议下执行
- **AND** 记录平均时延、p95、p99、I/O wait、提交次数、flush 次数、读字节与缺失 payload 次数

#### Scenario: submit-batch sweep
- **WHEN** 运行 submit-batch 消融
- **THEN** 运行 `submit-batch` 为 `4, 8, 16, 32`
- **AND** 每次运行均输出平均时延、p95、p99、提交次数、flush 次数、I/O wait 与队列压力证据
- **AND** 被阻塞值需写明阻塞原因

#### Scenario: safein_all_threshold 作为支持性验证
- **WHEN** 执行 `safein_all_threshold` 扫描
- **THEN** 对比 disabled/default/large 三档（若 runner 支持）
- **AND** 结果作为调度/队列压力支持性证据，而非主线必要结论

### Requirement: Thesis ablation study SHALL evaluate top-k sensitivity as a supplement
top-k 敏感性仅用于鲁棒性补充，不改写 `topk=10` 主矩阵结论。

#### Scenario: top-k 20 在双数据集补充
- **WHEN** 可获得 recall@20 输出/后处理
- **THEN** 在 COCO 与 MS MARCO 上执行 `topk=20` 补充
- **AND** 至少包含 BoundFetch、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`
- **AND** COCO SHALL 使用 `nprobe=16,32,64`
- **AND** MS MARCO SHALL 使用 `nprobe=32,64,128`
- **AND** baseline 重排先用 `candidate_budget=150`，若未达共同质量门槛再补跑 `200`

#### Scenario: top-k 50 与 top-k 100 作为压力证据
- **WHEN** recall@50 或 recall@100 暂不支持
- **THEN** 不将 top-k 50/100 作为主文据报
- **AND** 一旦支持，再采用 `topk=50` 的 `candidate_budget=250`、`topk=100` 的 `candidate_budget=500`
- **AND** COCO SHALL 使用 `nprobe=16,32,64`
- **AND** MS MARCO SHALL 使用 `nprobe=32,64,128` when MS MARCO stress runs are scheduled
- **AND** 在未提升到后续 change 前默认标注为 Appendix / stress 证据

### Requirement: Thesis ablation study SHALL keep outputs separate from thesis main results
消融结果与主实验输出必须可分离，不被主表自动吸收。

#### Scenario: 消融 run 使用独立追踪语义
- **WHEN** 记录任意消融或 top-k 补充运行
- **THEN** tracker 阶段标识为 result hygiene/triage ablation/scheduling ablation/top-k supplement
- **AND** 不参与主实验 main-sweep summary 选择
- **AND** 保留可追溯到对应主线参考运行点的关联

#### Scenario: 消融汇总按 claim 映射
- **WHEN** 形成消融汇总
- **THEN** triage 结果映射到 C2，调度结果映射到 C3，top-k 敏感性映射到 C4
- **AND** 区分 MUST-RUN、SHOULD-RUN 与 NICE 分支
- **AND** 缺失或跳过分支需显式记录而非静默省略
