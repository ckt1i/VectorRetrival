## Context

当前系统已经完成了 `redundant_top2` 的第一阶段落地：

- builder 支持 duplicated postings
- `.clu V9` 已支持 raw address table
- query path 能在 duplicated postings 下正确 dedup
- 端到端实验已经表明 `top-2 redundant` 可以降低高 recall 所需的 coarse probing 压力

但当前 secondary assignment 仍然采用 naive 规则，即在 primary cluster 之外直接选择“第二近 centroid”。这一规则已经写死在 [src/index/ivf_builder.cpp](/home/zcq/VDB/VectorRetrival/src/index/ivf_builder.cpp) 的 `DeriveSecondaryAssignments()` 中，只考虑 `||x-c'||^2`，没有利用 residual direction 信息。

RAIRS 的 RAIR 部分给出一个更适合 Euclidean 空间的 secondary-list 选择规则：

- 记 primary residual 为 `r = c1 - x`
- 候选 secondary residual 为 `r' = c' - x`
- 使用 `AIR(r') = ||r'||^2 + lambda * <r, r'>` 作为 loss

其中第二项鼓励 secondary centroid 从与 primary centroid 不同的方向覆盖 query space，从而减少高 recall 区间对大 `nprobe` 的依赖。

当前 change 的约束很明确：

- 只吸收 RAIR，不做 SEIL
- 不改 query 扫描核心，不引入 shared-cell block layout
- 继续沿用当前 `.clu V9 + duplicated postings` 方案
- duplicated postings 仍直接写成两份 cluster-side posting，继续共享同一原始向量语义

## Goals / Non-Goals

**Goals:**
- 在 builder 侧新增显式的 RAIR secondary-assignment 策略。
- 保留 `single` 和 `naive redundant_top2`，形成三种 assignment mode 的统一构建入口。
- 将 RAIR 关键参数暴露到 build config、metadata 和 benchmark CLI。
- 设计一套实验，回答 RAIR 是否比 naive `top-2` 进一步降低高 recall 下所需的 `nprobe`。
- 在当前 serving path 不变的前提下，给出 RAIR vs naive 的端到端对照结果。

**Non-Goals:**
- 不实现 SEIL/shared-cell block layout。
- 不更改 `.clu` 地址格式或 `.data` 存储语义。
- 不修改 query 侧 probe kernel、CRC 公式或 FastScan 内核。
- 不实现 `nrep > 2` 的多归属扩展。

## Decisions

### 1. Secondary assignment mode 显式化，而不是复用 `assignment_factor`

当前 `assignment_factor=2` 只表达“是否做 top-2 冗余”，但不表达“第二个 list 怎么选”。这会让：

- `naive redundant_top2`
- `RAIR redundant_top2`

在 metadata 和 benchmark 结果里不可区分。

因此新增一个独立枚举，例如：

- `single`
- `redundant_top2_naive`
- `redundant_top2_rair`

`assignment_factor` 仍可保留为派生字段，但不再作为唯一语义来源。

选择这个方案而不是仅新增一个 `bool use_rair`，是为了让：

- metadata 可直接反映构建语义
- benchmark 结果可直接按 mode 分组
- 后续如果要加入 SOAR，不需要再推翻字段设计

### 2. RAIR 只替换 secondary-list 选择，不改 primary assignment

primary assignment 继续保持“最近 centroid”：

- 兼容现有 IVF 训练与 cluster semantics
- 不影响现有 single-assignment 路径
- 让 RAIR 的影响只集中在 secondary posting

也就是说：

- `assignments_[i]` 继续表示 primary cluster
- `secondary_assignments_[i]` 在 RAIR 模式下由 AIR loss 导出

这样可以最小化改动范围，同时保持当前 writer / metadata / query path 的兼容性。

### 3. RAIR loss 直接在 builder 内按 residual 计算

对每个向量 `x`：

- 先确定 primary cluster `c1`
- 计算 `r = c1 - x`
- 对每个候选 `c' != c1` 计算 `r' = c' - x`
- 用 `||r'||^2 + lambda * <r, r'>` 选最小者

这一步直接在 `DeriveSecondaryAssignments()` 内完成，不新增外部依赖，也不引入单独的训练阶段。

理由：

- 当前 builder 已经遍历全部 centroids 求最近/次近
- residual 所需信息 builder 全部已有
- 先把 RAIR 作为 deterministic builder rule 落地，最容易和当前 naive 结果直接对比

备选方案是只在 top-k centroid 候选池内近似求 RAIR，以降低 build 成本。当前不采用，因为：

- `nlist=1024/2048` 下全扫描 build 成本仍可接受
- 首轮实验更需要规则本身正确，而不是先优化 build 时间

### 4. `strict-second-choice` 和 `lambda` 作为显式参数暴露

RAIRS 仓库里有两个关键行为开关：

- `lambda`
- `strict_redund_mode`

这里建议直接暴露到 builder config 和 benchmark CLI：

- `--assignment-mode`
- `--rair-lambda`
- `--rair-strict-second-choice`

默认值建议：

- `assignment-mode=single`
- `rair-lambda=0.75`
- `rair-strict-second-choice=0`

这样可以：

- 先用论文默认语义重现实验
- 后续如果 RAIR 有潜力，再单独做 lambda sweep

### 5. 当前 serving path 保持不变，RAIR 只改变 posting membership

RAIR 的首轮目标是验证 coarse partition 改善是否转化为：

- 更低的高-recall 所需 `nprobe`
- 更好的 recall-latency trade-off

因此 query 侧继续复用当前：

- `.clu V9`
- full preload
- duplicated posting dedup
- overlap scheduler

这能保证实验结论只归因于“第二个 list 选得更好”，不会和 query-path 其他变化混在一起。

### 6. Benchmark 分两层：固定 `nprobe=512` 的 E2E 对照 + 可变 `nprobe` 的 high-recall sweep

RAIR 是否有效，不能只看固定 `nprobe=512` 的 E2E。

需要两层评估：

1. 固定 operating point 的端到端对照
   - `single`
   - `naive redundant_top2`
   - `rair redundant_top2`

2. 可变 `nprobe` 的 high-recall sweep
   - 目标是回答“达到 `0.99 recall` 所需 `nprobe` 是否下降”

只看第一层会把 RAIR 的收益掩盖在“候选更多、probe 成本也更高”的现象里；第二层才能直接回答 partition-quality 问题。

## Risks / Trade-offs

- [RAIR 在当前数据分布上收益有限] → 先固定 `lambda=0.75` 做小规模 sweep，若对高 recall 所需 `nprobe` 改善不明显，则停止这条线，不推进更复杂的 list-layout 优化。
- [Builder 成本上升] → 首轮接受 builder 全扫描的额外 CPU 成本，只记录 build time；若 serving 收益成立，再考虑候选池近似。
- [Metadata/benchmark 语义混乱] → 将 `assignment_mode` 作为一等字段写入 `segment.meta` 和 benchmark 输出，而不是从 `assignment_factor` 推断。
- [与现有 redundant change 范围耦合] → 复用 `.clu V9` 和 duplicated posting 路径，但本 change 不再修改地址格式与 query 内核，避免和存储变更混合。
- [RAIR 只改善 coverage，不一定改善固定 `nprobe` 延迟] → 评估标准明确拆成“固定点 E2E”与“目标 recall 所需 `nprobe`”两类，不用单一指标做结论。

## Migration Plan

1. 在 builder config 中加入 `assignment_mode`、`rair_lambda`、`rair_strict_second_choice`。
2. 保留旧的 `single` 和 `redundant_top2_naive` 语义，作为回退路径。
3. `segment.meta` 记录新的 assignment mode 字段；旧索引仍按现有 metadata 兼容读取。
4. benchmark CLI 增加 RAIR 相关参数，并在 aggregate output 中导出 mode 和 lambda。
5. 先在 `coco_100k` 上执行：
   - `2048 single`
   - `2048 redundant_top2_naive`
   - `2048 redundant_top2_rair`
6. 若 RAIR 证明能降低高 recall 所需 `nprobe`，再扩展到 `1024` 线；否则停止。

Rollback:

- 构建时切回 `assignment_mode=single` 或 `redundant_top2_naive`
- 不删除当前 duplicated posting / `.clu V9` 路径

## Open Questions

- `lambda` 是否直接采用论文默认 `0.75`，还是需要先做一个极小范围的 `{0.5, 0.75, 1.0}` sanity sweep？
- `strict_second_choice=0` 是否在当前数据上足够，还是需要同时报告 `strict=1` 的一条辅助线？
- `1024` 线是否需要和 `2048` 同步做 RAIR，还是等 `2048` 先证明有效后再扩展？
