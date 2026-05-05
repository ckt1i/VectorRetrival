## Context

论文主实验 `T000-T015` 已完成。剩余工作不再是另一轮主线基线扩展，而是把主实验结果固化为可解释的机制证据。

最新实验计划的后主实验区块包括：

- 结果卫生：重跑或标注 MS MARCO 的异常 `nprobe=256` 尾延迟点并冻结匹配质量代表点。
- triage 消融：隔离 SafeIn、SafeOut、Uncertain 的贡献。
- 调度消融：隔离动态 io_uring 提交策略对尾延迟的作用。
- top-k 补充：验证 top-k 增大后的趋势稳定性。

主线 change 已仅负责 `topk=10` 的主矩阵、canonical artifact 复用和主结果跟踪，本 change 只负责后续补充实验。

本轮进一步冻结消融运行点：

- 除 top-k 以外的消融实验固定为 COCO `nprobe=64`，MS MARCO `nprobe=128`。
- top-k 对比实验在 COCO 使用 `nprobe=16,32,64`，在 MS MARCO 使用 `nprobe=32,64,128`。
- triage 消融允许为实验添加最小必要补丁，但必须记录 SafeIn/SafeOut/Uncertain 比率，作为后续结果分析的参考。

本 change 继承主实验 change 的实验口径：

- 数据集只使用 `coco_100k` 与 `msmarco_passage`。
- 不新增主表 baseline family；top-k 补充至少覆盖 BoundFetch、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`。
- `queries=1000`。
- BoundFetch 主线默认保持 `crc=1`、`early-stop=0`、`bits=4`、`skip_gt=0`。
- 正式 BoundFetch measurement 继续使用 warmup-then-measurement 协议：每个 BoundFetch 正式运行点必须先跑一次同参数 warmup，再跑 measurement；warmup 不进入论文结果。
- 所有运行必须复用对应数据集的 canonical artifact，禁止重新从 raw vectors 训练、聚类或生成 assignment。
- 每个结果必须保留 dataset、system、nprobe、topk、canonical artifact、warmup/measurement、`crc`、`early-stop`、`skip_gt` 等 provenance 字段。

## Goals / Non-Goals

**Goals:**

- 把后主实验的 ablation study 用统一合同固化，支持论文 C2、C3、C4 结论。
- 优先采用主实验已冻结的 artifact 和 representative point，不再重复完整矩阵。
- 继承主实验的参数设置、canonical artifact 复用、strict recall 和 warmup/measurement 口径。
- 所有消融输出需同时输出 recall、latency 与 I/O/triage/payload 关键计数。
- 将 `topk=20` 作为主推荐补充实验，
- 将 `topk=50/100` 作为压力或附录补充。
- 保持主表与消融证据的分离。

**Non-Goals:**

- 不把 `topk=10` 主表扩展到其他 top-k。
- 不新增 DiskANN、HNSW、ScaNN、IVFRQ、IVFPQR 等基线族。
- 不加入 synthetic payload、Deep1M/Deep8M、Amazon ESCI 等附录数据集。
- 不把 `crc=0`、early-stop 变体、RAIR 或 duplicate clustering 当作主线行为。
- 不允许为了消融方便而绕过 canonical artifact、关闭 strict recall 或混用 warmup 输出。

## Decisions

### 1. 非 top-k 消融采用固定代表点，不做完整 sweep

除 top-k 以外的机制消融采用固定代表点执行：COCO 固定 `nprobe=64`，MS MARCO 固定 `nprobe=128`。这两个点作为机制分析点，不再根据每轮结果临时漂移。

备选方案：

- 全量跑每个 `nprobe`：证据更密，但成本高，且对本科论文周期不匹配。
- 只在 COCO 跑：成本低，但跨 workload 的机制外推性不足。

### 2. triage 消融采用删除式对照，并记录三分类比例

主实验满量策略对比：

- SafeOut 剪枝关闭
- SafeIn payload 预取关闭
- Uncertain eager payload

边界参数/再分类的敏感性分析作为可选增强，只在已有参数可用时引入。若当前实现缺少必要实验 flag，允许添加最小补丁完成实验，但补丁范围只应覆盖实验开关和结果输出，不引入新的方法机制。

每个 triage 运行必须记录 SafeIn/SafeOut/Uncertain 比率。该比例不是单独的因果消融结论，但用于解释删除式消融中 rerank、payload read、bytes read 和 tail latency 的变化。

备选方案：

- 只看 triage 比率：不够支撑因果。
- 扫所有阈值参数：与论文主线贡献不如删除式对照集中。

### 3. 调度消融聚焦两个核心维度

必须完成：

- `submit-online` on/off
- `submit-batch` 在 `4,8,16,32`

`safein_all_threshold` 作为支持性证据（SHOULD）。

备选方案：

- 将调度视为实现细节：会削弱 C3 的论证强度。
- 再扩展 queue-depth、prefetch 维度：范围过大，收益递减。

### 4. top-k 为鲁棒性补充，不作为主实验

主表仍以 `topk=10` 为准。top-k 对比不再只跑单个 representative point，而是采用小型 nprobe sweep：COCO 使用 `nprobe=16,32,64`，MS MARCO 使用 `nprobe=32,64,128`。`topk=20` 为推荐补充，`topk=50/100` 为压力实验。

### 5. 结果卫生先行

MS MARCO `nprobe=256` 异常点与 matched-quality 冻结是后续解释性实验的前置条件。

### 6. ablation 继承主实验执行原则

消融与 top-k 补充不是新的实验系统，因此必须沿用主实验的公平性约束：同一数据集使用同一 canonical artifact，同一 BoundFetch 运行点必须先同参数 warmup 再 measurement，正式结果必须 `skip_gt=0` 并输出 recall。任何非主线参数组合只能作为 ablation/debug 记录，不能替代 BoundFetch-Guarded 主线。

备选方案：

- 为每个消融重建索引：实现上可能方便，但会重新引入 builder 差异，不接受。
- 允许 warmup 结果进入图表：会混入冷读/初始化成本，不接受。
- 跳过 BoundFetch warmup 直接 measurement：会把冷启动或首次 io_uring 读路径混入尾延迟，不接受。
- 为 top-k 补充加入更多 baseline family：会把补充实验变成第二套主实验，不接受。

## Risks / Trade-offs

- 缺少消融 flag：优先用现有 proxy 参数；仅在无法隔离时最小化新增控制。
- top-k recall 尚未输出：延后 top-k 实验，不提前报表。
- 消融增益较小：降低 triage/scheduler 结论强度，改为“辅助优化”叙述。
- 主实验尾延迟异常点：需复核后再写 p99 结论。
- 混用冷启动与 warm-serving：按协议严格区分。
- 消融实现绕过 canonical artifact：运行前检查 artifact provenance，缺失则不纳入论文证据。

## Migration Plan

1. 固定非 top-k 消融点：COCO `nprobe=64`，MS MARCO `nprobe=128`。
2. 确认每个消融运行都能复用主实验 canonical artifact，并保留 provenance。
3. 确认消融与 top-k 所需 flag 和指标可用性。
4. 对每个正式 BoundFetch 运行点先执行同参数 warmup，并把 warmup run id 记录为 measurement provenance。
5. 在 COCO 和 MS MARCO 固定点上完成 triage 消融，并记录三分类比例。
6. 完成 COCO 调度消融。
7. 仅在 top-k recall 可用后执行 top-k 结果补充，COCO 使用 `16,32,64`，MS MARCO 使用 `32,64,128`。
7. 输出独立消融汇总，对应 C2/C3/C4。

## Open Questions

- SafeOut 关闭、SafeIn prefetch 关闭、Uncertain eager 是否已有可用 CLI；如无，是否允许最小补丁？
- top-k 对比的 `topk=50/100` 是否只在 COCO 报告，还是在 MS MARCO 上也作为附录压力实验补跑？
