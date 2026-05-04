## 1. 冻结消融输入与结果卫生

- [ ] 1.1 固定所有 ablation 继承参数：`queries=1000`、BoundFetch `crc=1`、`early-stop=0`、`bits=4`、正式 measurement `skip_gt=0`。
- [ ] 1.2 固定非 top-k 消融运行点：COCO 使用 `nprobe=64`，MS MARCO 使用 `nprobe=128`。
- [ ] 1.3 确认所有消融和 top-k 补充运行复用对应数据集的 canonical artifact，且不重新训练/聚类/生成 assignment。
- [ ] 1.4 在形成 p95/p99 叙述前，重跑或明确标注 MS MARCO `nprobe=256` 的异常点。
- [ ] 1.5 为每个被选主线点记录数据集、系统、后端、canonical artifact、nlist、nprobe、topk、recall、latency 和来源 run id。
- [ ] 1.6 验证消融任务记录到 tracker 时使用 hygiene/triage/scheduling/top-k supplement 阶段标识，避免写入主表。
- [ ] 1.7 验证 warmup 输出和正式 measurement 输出分离，且 summary 只使用 measurement 结果。

## 2. 校验消融控制与指标输出

- [ ] 2.1 检查 SafeOut 关闭、SafeIn 预取关闭、Uncertain eager 是否已有现有 CLI。
- [ ] 2.2 若现有 flag 不足，添加最小实验补丁暴露缺失控制，且不得改变主线默认行为。
- [ ] 2.3 校验输出包含 SafeIn/SafeOut/Uncertain、rerank 数、payload 预取、payload 缺失、payload 读取、读字节与 recall。
- [ ] 2.4 校验输出包含 I/O wait、submit 次数、flush 次数、p95/p99。
- [ ] 2.5 在运行 top-k 实验前确认 recall@20/50/100 可复用。
- [ ] 2.6 校验所有输出保留 `crc`、`early-stop`、`skip_gt`、warmup/measurement、canonical artifact provenance。

## 3. 执行 triage 删除式消融

- [ ] 3.1 在 COCO `nprobe=64` 执行 SafeOut 剪枝关闭。
- [ ] 3.2 在 COCO `nprobe=64` 执行 SafeIn payload 预取关闭。
- [ ] 3.3 在 COCO `nprobe=64` 执行 Uncertain eager payload。
- [ ] 3.4 在 MS MARCO `nprobe=128` 执行完整策略与关键删除变体确认。
- [ ] 3.5 做 triage 差异聚合：recall、时延、rerank、payload 读、读字节、SafeIn/SafeOut/Uncertain 比率。

## 4. 执行动态调度消融

- [ ] 4.1 在 COCO `nprobe=64` 跑 `submit-online=0/1`。
- [ ] 4.2 在 COCO `nprobe=64` 跑 `submit-batch=4,8,16,32`。
- [ ] 4.3 若支持，跑 `safein_all_threshold` disabled/default/large。
- [ ] 4.4 聚合调度差异：平均时延、p95、p99、I/O wait、submit 次数、flush、队列压力。

## 5. 执行 top-k 补充

- [ ] 5.1 COCO `topk=20`：BoundFetch、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`，`nprobe=16,32,64`。
- [ ] 5.2 MS MARCO `topk=20`：BoundFetch、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`，`nprobe=32,64,128`。
- [ ] 5.3 recall@50 支持后再跑 COCO top50（`candidate_budget=250`，`nprobe=16,32,64`）。
- [ ] 5.4 recall@100 支持后再跑 COCO top100（`candidate_budget=500`，`nprobe=16,32,64`）。
- [ ] 5.5 top-k 补充结果不得进入 `topk=10` 主表汇总。

## 6. 输出消融汇总供写作

- [ ] 6.1 输出 triage 消融图/表并映射到 C2。
- [ ] 6.2 输出调度消融图/表并映射到 C3。
- [ ] 6.3 输出 top-k 敏感性图/附录表并映射到 C4。
- [ ] 6.4 标注未跑/跳过的 SHOULD/NICE 分支原因。
- [ ] 6.5 更新论文写作说明，主实验与消融为独立小节。
