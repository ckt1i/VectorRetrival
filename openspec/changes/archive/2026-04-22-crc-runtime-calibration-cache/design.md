## Context

`crc-runtime-calibration-presort` 完成之后，CRC runtime calibration 的热点已经从 `ComputePredictions + sort` 转移到了 `BuildStopProfiles`。最新 perf 表明，preparation 阶段的剩余成本主要由三部分构成：

1. 对同一 subset 和同一 `QueryScores`，在 calib / tune / test 以及多个 `reg_lambda` 之间重复计算 step-level overlap  
2. `RegularizeScores` 对固定 `nonconf` 的 query 在每个 `reg_lambda` 下重复做 rank 排序  
3. `BuildStopProfiles` 在 profile 构建阶段仍有较多小对象分配与临时容器成本

这些工作都发生在 CRC prepare 阶段，而不是在线 query 阶段。当前线上 `CrcStopper::ShouldStop()` 已经是 O(1) 消费 calibration 结果，所以这次 change 只针对 runtime calibration preparation 继续收敛，不触碰 query 主路径和 solver 语义。

## Goals / Non-Goals

**Goals:**
- 将与 `reg_lambda` 无关的 step-level 统计提升为 subset-level 静态 cache，一次构造，多处复用。
- 将 `RegularizeScores` 中的 rank 排序改为固定 subset 上的一次性预计算，避免不同 `reg_lambda` 之间重复排序。
- 收敛 `BuildStopProfiles` 的布局与分配方式，使 profile 构建更接近“投影已有 cache + 排序结果”，而不是重新做完整统计。
- 保持 Brent 求解、`lamhat` 语义、`CalibrationResults` 输出、`crc_scores.bin` 格式与线上 `CrcStopper` 行为不变。

**Non-Goals:**
- 不切换为离散候选阈值搜索。
- 不改变 `kreg` / `reg_lambda` 的搜索空间。
- 不修改 `crc_scores.bin` 的生成方式或序列化格式。
- 不优化在线 query 主路径，也不修复 offline/online stop 语义差异。

## Decisions

### Decision: 将 calibration subset 拆成静态 cache 与 reg-lambda 相关 profile 两层
新的内部组织应拆成两类数据：

- `SubsetStaticCache`
  - 每条 query 的 `gt_size`
  - 每个原始 `step` 的 `overlap_by_step`
  - 每条 query 的 `nonconf`
  - 每个原始 `step` 对应的 `rank_by_step`
- `RegLambdaProfiles`
  - 给定 `reg_lambda/kreg` 后生成的 `reg_score`
  - `(reg_score, step)` 升序排序结果
  - 排序位置对应的 `clusters_searched`

也就是说：

```text
subset 固定后:
  GT / overlap / nonconf / rank 只算一次

每个 reg_lambda:
  只做公式计算 + 排序 + profile 投影
```

这样做的原因是，当前 perf 已经证明 overlap 计算和 rank 排序仍在重复发生；而它们本身不依赖 `reg_lambda`，应当从 `BuildStopProfiles` 和 `RegularizeScores` 里抽出来。

### Decision: overlap cache 以原始 step 为索引，而不是随 sorted profile 重复生成
静态 cache 应保存：

```text
overlap_by_step[q][step]
```

而不是直接保存 `overlap_at_sorted`。  
`BuildStopProfiles` 在得到 `sorted_to_step` 后，只需做一次投影：

```text
overlap_at_sorted[i] = overlap_by_step[sorted_to_step[i]]
```

这样静态 cache 可以在多个 `reg_lambda` 和多个 profile 之间共享，不会因为排序顺序不同而重复重算 overlap。

### Decision: rank 预计算单独建表，`RegularizeScores` 只负责公式展开
当前 `RegularizeScores` 对每条 query 都会先按 `nonconf` 降序排序，得到 rank，再代入：

```text
E = (1 - nonconf) + reg_lambda * max(0, rank - kreg)
```

新的实现应将“按 `nonconf` 排序得到 rank”前移，预计算：

```text
rank_by_step[q][step]
```

之后 `RegularizeScores` 对任意 `reg_lambda` 只做纯公式计算，不再重复排序。  
选择这一方案是因为 rank 与 `reg_lambda` 无关，是最典型的可复用静态数据。

### Decision: profile 构建优先优化数据流，其次再收敛内存布局
本次 change 优先把数据流变成：

```text
static cache
  + reg_scores
  -> sorted_to_step
  -> projected overlap / clusters
```

然后再顺手减少 profile 构建中的小对象开销，例如：
- 尽量复用索引数组或 entry buffer
- 降低 `vector<pair<...>>` 的临时分配次数
- 视代码可读性决定是否把多个并行 vector 收敛为紧凑 entry 数组

之所以不一开始就强推结构体数组，是因为这一层属于常数项优化，应建立在数据流先拆干净的基础上。

### Decision: 验收以热点迁移和行为稳定双重约束为主
本次 change 的验收标准不是单纯“时间更短”，而是：
- `BuildStopProfiles` 不再承担重复 overlap 计算
- `RegularizeScores` 不再重复 `nonconf` 排序
- `lamhat / actual_fnr / avg_probed / recall@10` 与当前实现保持等价或近似等价
- perf 中 preparation 热点进一步向真正不可避免的排序/solver 成本收敛

这一定义能确保我们不是通过引入 solver 语义漂移来换取性能。

## Risks / Trade-offs

- [静态 cache 让代码层次更复杂] → 通过明确 `SubsetStaticCache` 与 `RegLambdaProfiles` 的边界，避免“所有统计都混在一个 profile 里”。
- [cache 增加内存占用] → 只为当前 calib / tune / test 子集持有 cache，不复制完整预测集，也不持久化到磁盘。
- [rank 预计算与现有 `RegularizeScores` 的排序规则出现偏差] → 保持与当前实现一致的排序方向和同分处理规则，并用固定 `crc_scores.bin` 做数值回归。
- [布局优化影响可读性] → 优先拆静态 cache 与数据流；只有在 perf 仍显示分配成本显著时，再进一步压缩 entry 布局。
- [做完 cache 后 Brent 仍可能是主要剩余成本] → 接受这是本次 change 的结果边界；若 solver 本身继续主导，再单独开离散候选阈值搜索 change。
