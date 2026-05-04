# 主实验完成后的实验计划再细化建议

**日期**: 2026-05-03  
**范围**: 基于已完成主实验、`EXPERIMENT_TRACKER.md`、`THESIS_BLUEPRINT.md` 以及 `/home/zcq/VDB/baselines/results/formal-study/` 的结果，判断实验计划是否需要更新、是否需要追加消融。  
**约束**: 本文档不修改 `openspec/changes/thesis-minimal-main-sweep`。

## 结论概览

需要更新实验计划，但不需要扩大主实验矩阵。

当前主实验已能支撑 RQ1：在 COCO100K 和 MS MARCO 上，BoundFetch-Guarded 都快于保留 baseline，且对最有意义的对比组 `IVF+RaBitQ` 也成立。下一阶段应从“继续扩主 sweep”切换为“机制证明与结果固化”：

1. 固化主实验曲线与 matched-quality 主表。
2. 补齐 RQ2 的三分类机制消融。
3. 补齐 RQ3 的调度机制消融。
4. 选择性补齐 RQ4 的大 top-k 敏感性。
5. 将当前本科论文不再使用的 synthetic payload 计划降级或移出。

## 主实验证据

结果来源：

- `/home/zcq/VDB/baselines/results/formal-study/thesis_main_experiment_conclusions.md`
- `/home/zcq/VDB/baselines/results/formal-study/thesis_main_experiment_plot_table_points.csv`

### COCO100K

BoundFetch sweep：

| nprobe | recall@10 | e2e ms |
|---:|---:|---:|
| 16 | 0.8304 | 0.6720 |
| 32 | 0.9127 | 0.8247 |
| 64 | 0.9582 | 1.1307 |
| 128 | 0.9835 | 1.6888 |
| 256 | 0.9947 | 2.7669 |
| 512 | 0.9986 | 4.5459 |

要点：

- `IVF+PQ` 在该数据集上 recall 上限约 0.7445，属于弱基线。
- 有意义的强对比是 `IVF+RaBitQ`。
- 对 `IVF+RaBitQ+FlatStor`，BoundFetch 在相近 recall 区间约有 `2.67x / 1.99x / 1.05x` 加速（低/中/高 recall 区间）。
- 对 `IVF+RaBitQ+Lance`，BoundFetch 加速更明显。

### MS MARCO Passage

BoundFetch sweep：

| nprobe | recall@10 | e2e ms | p99 ms |
|---:|---:|---:|---:|
| 16 | 0.7786 | 7.3108 | 17.9266 |
| 32 | 0.8443 | 7.2875 | 14.5896 |
| 64 | 0.8934 | 9.7796 | 30.4943 |
| 128 | 0.9320 | 9.0271 | 15.4166 |
| 256 | 0.9603 | 32.8081 | 304.4682 |
| 512 | 0.9763 | 13.8631 | 26.3463 |
| 1024 | 0.9895 | 20.0816 | 37.2204 |

要点：

- `IVF+PQ` 在该数据集 recall 上限约 0.6472，仍属弱基线。
- 对 `IVF+RaBitQ+FlatStor`，BoundFetch 在相近 recall 区间约有 `8.74x / 7.08x / 3.64x` 加速。
- 对 `IVF+RaBitQ+Lance`，BoundFetch 在相近 recall 区间约有 `5.91x / 4.76x / 2.53x` 加速。

注意事项：

- MS MARCO 的 `nprobe=256` 出现异常尾延迟（`p99=304.4682 ms`），而 `nprobe=512` 反而更低。该点不影响总体“BoundFetch 更快”结论，但若要在论文里写 tail-latency 机制，建议重跑或显式标注为 noisy point。

## RQ 覆盖度复核

| Research Question | 当前状态 | 需要补充 |
|---|---|---|
| RQ1: BoundFetch 是否降低 matched-quality E2E latency | 已覆盖 | 固化主表与曲线 |
| RQ2: SafeIn/SafeOut/Uncertain 哪部分贡献最大 | 未覆盖 | 补机制删除型消融 |
| RQ3: 动态异步调度是否改善 tail latency | 未覆盖 | 补调度消融 |
| RQ4: 更大 top-k 是否仍成立 | 未覆盖 | 先补 top20，再视时间补 top50/top100 |

结论：实验计划应做“主实验后追加说明（addendum）”，方法主线本身无需重写。

## 建议更新项

### 更新 1：将主 sweep 正式冻结为“已完成”

将 T000-T015 从“执行计划”转为“结果固化”阶段。

需要完成：

- 产出 COCO 与 MS MARCO 的最终 recall@10 vs E2E latency 曲线。
- 固化 matched-quality 汇总主表。
- 论文叙事中把 `IVF+RaBitQ` 作为最强基线组。
- `IVF+PQ` 保留在主表，但注明其高 recall 区间不可达。
- Lance 作为 benchmark/reference backend，不作为主后端叙事。

### 更新 2：加入结果清洗步骤

在写结论前补一个轻量校验阶段：

| 新增 ID | 目的 | 数据集 | 系统 | 参数 | 优先级 |
|---|---|---|---|---|---|
| T028 | 重跑 MS MARCO 异常点 | msmarco_passage | BoundFetch-Guarded | nprobe=256, topk=10 | SHOULD |
| T029 | 固化 matched-quality 选点 | 双数据集 | 全保留系统 | 统一选 Q* 与代表点 | MUST |

原因：

- MS MARCO 的异常 p99 点不应直接进入 tail-latency 机制结论。
- 消融实验应绑定稳定、可解释的 operating point。

### 更新 3：将三分类消融提升为最高优先级

RQ1 已成立，当前最缺机制证据，建议最小集合如下：

| 现有 ID | 消融项 | 数据集 | operating point | 优先级 | 作用 |
|---|---|---|---|---|---|
| T016 | SafeOut pruning off | COCO100K | nprobe=64/128 | MUST | 证明 pruning 降低 rerank/read 放大 |
| T017 | SafeIn payload prefetch off | COCO100K | nprobe=64/128 | MUST | 证明提前 payload 读取的收益 |
| T018 | Uncertain eager payload | COCO100K | nprobe=64/128 | MUST | 证明 delayed payload 避免无效读取 |

推荐点位：

- COCO nprobe=64（recall 0.9582，e2e 1.1307 ms）
- COCO nprobe=128（recall 0.9835，e2e 1.6888 ms）
- MS MARCO nprobe=128 可选做 1 个跨数据点补充

建议统一报告指标：

- recall@10
- e2e、p95、p99
- SafeIn/SafeOut/Uncertain 比例
- reranked candidates
- bytes read
- payload-prefetched reads
- final missing payload fetches

### 更新 4：保留调度消融，但缩成小矩阵

调度消融用于回答 RQ3，不建议再做全量大 sweep。

推荐最小集合：

| 现有 ID | 消融项 | 数据集 | operating point | 优先级 |
|---|---|---|---|---|
| T021 | submit-online on/off | COCO100K | nprobe=64 或 128 | MUST |
| T022 | submit-batch sweep | COCO100K | nprobe=64 或 128 | MUST |
| T023 | safein_all_threshold sweep | COCO100K | nprobe=64 或 128 | SHOULD |

`submit-batch` 建议值：`4, 8, 16, 32`。

除非 COCO 结论不稳定，否则不要在两个数据集都重复这组调度消融。

### 更新 5：大 top-k 敏感性在输出能力就绪后再跑

RQ4 必要，但不能阻塞主线。

建议顺序：

| 现有 ID | 实验 | 数据集 | 优先级 |
|---|---|---|---|
| T024 | topk=20 sensitivity | COCO 优先，MS MARCO 视输出能力追加 | SHOULD |
| T025 | topk=50 stress | COCO | NICE |
| 新增 | topk=100 stress | COCO | NICE / appendix |

前置条件：

- benchmark 或后处理需能稳定给出 recall@20 / recall@50 / recall@100。
- 若当前只有 recall@10，就先补输出能力，再做大 top-k。

### 更新 6：当前本科阶段移出 payload sensitivity

T026 建议从当前本科计划移出，或标记为 out-of-scope。

原因：

- 当前 `THESIS_BLUEPRINT.md` 已不再把 synthetic payload 作为主线。
- 两个真实数据集已能支撑主结论。
- 资源应优先给 RQ2/RQ3 的机制证据。

## 时间紧张时的最小追加集合

| 优先级 | 运行项 | 数据集 | 点位 |
|---|---|---|---|
| P0 | matched-quality 主表固化 | COCO + MS MARCO | 主 sweep 全点 |
| P0 | SafeOut off | COCO | nprobe=64/128 |
| P0 | SafeIn prefetch off | COCO | nprobe=64/128 |
| P0 | Uncertain eager payload | COCO | nprobe=64/128 |
| P1 | submit-online on/off | COCO | 同点位 |
| P1 | submit-batch sweep | COCO | 同点位 |
| P1 | topk=20 | COCO 优先，MS MARCO 视情况 | matched 区间 |

该集合即可支撑：

- RQ1：主 sweep 已完成
- RQ2：三分类机制消融
- RQ3：调度机制消融
- RQ4：至少 top20 敏感性

## 不建议追加的内容

当前阶段不建议新增 baseline 家族。

原因：

- 现有四组保留 baseline 下主结论已明确。
- 最强有效对比已经是 `IVF+RaBitQ`。
- 新增 graph/disk ANN 会扩大范围但不增强当前论文核心结论。

当前阶段不建议把 Lance 改成主故事。

原因：

- 当前论文框架明确 FlatStor 为主后端，Lance 为 benchmark/reference backend。
- Lance 结果有价值，但不是主结论依赖项。

## 对 Tracker 的建议补丁

若更新 `EXPERIMENT_TRACKER.md`，建议：

1. T000-T015 保持 `DONE`
2. 新增 T028（MS MARCO nprobe=256 异常点复核）
3. 新增 T029（matched-quality 主表/曲线固化）
4. T016-T018 保持 `MUST`
5. T021-T022 保持 `MUST`
6. T023 调整为 `SHOULD`
7. T024 保持 `SHOULD`
8. T025 调整为 `NICE`
9. T026 标记为 `OUT-OF-SCOPE`（当前本科阶段）
10. 后续 tracker 指标描述移除 MRR/nDCG 口径，统一 ANN recall 口径

## 最终建议

实验计划应更新，但形式应是“主实验完成后的追加说明”，而不是新起一份全量实验计划。

当前最必要的追加实验是机制导向的：

- SafeOut off
- SafeIn prefetch off
- Uncertain eager payload
- submit-online / submit-batch 调度消融
- 输出能力就绪后补 top20（再决定 top50/top100）

这组追加项规模可控，并且和你在 `THESIS_BLUEPRINT.md` 里的当前 RQ 完全对齐。
