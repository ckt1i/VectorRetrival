## Context

当前仓库中的 BoundFetch 查询路径已经包含大量与论文主方法强相关的优化，例如 overlap scheduling、CRC early stop、cluster preload、payload 协同提取等。这些优化如果直接参与 `IVF+RQ+rerank` baseline，会让 baseline 变成“主系统的弱配置”，而不是一条独立、可解释的对照线。

用户当前希望引入的 baseline 来自官方 `RaBitQ-Library`，并明确要求：

- 实现放在 `/home/zcq/VDB/third_party`
- coarse clustering / probe 行为直接复用或参考 Faiss
- 最终输出采用三层分离的磁盘存储方案
- CLI 需要显式暴露 `nlist`、`nprobe`、输出路径等参数，便于适配现有实验计划

因此，这个 change 的核心不是把 RaBitQ 嵌回主仓库，而是建立一条独立的、最简而稳定的 baseline pipeline。

在 baseline 工程已经完成后，新的问题出在 formal-study wrapper 的指标定义：`run_vector_search/ivf_rabitq_rerank.py` 当前用的是 hit-style 成功率

```python
if p & g:
    hits += 1
return hits / len(pred_ids)
```

这表示“每个 query 的预测 top-k 只要命中任意一个 GT top-k 就记为 1”，而不是标准的 top-k overlap recall。主仓库 `bench_e2e` 的定义是：

```text
recall@k(query) = |pred_topk ∩ gt_topk| / k
```

两者不能混用。当前 design 需要补的，不是换索引结构，而是把 `IVF+RQ+rerank` baseline 的评测 contract 改成与主仓库一致。

## Goals / Non-Goals

**Goals:**
- 在 `third_party` 下提供一个独立的 `IVF + RaBitQ + exact rerank` baseline 工程
- 采用三层分离架构：
  - compressed index plane
  - raw vector plane
  - payload plane
- 使用 Faiss 完成 coarse quantizer 训练、聚类分配和 probing 参考逻辑
- 使用官方 `RaBitQ-Library` 的 IVF index 作为 compressed search 主索引
- 提供 build/search 两类 CLI，并暴露 formal-study 需要的关键参数
- 输出稳定、显式的磁盘文件布局，便于后续 profile、debug 和复现实验
- 第一版结果可以直接接入现有 formal-study runner 或其后续包装层
- `IVF+RQ+rerank` 的 `recall@10`、`recall@20` 必须与主仓库和分析文档保持同一语义
- 将 hit-style 成功率保留为显式辅助指标，而不是继续冒充 recall
- 让结果能够区分 coarse candidate 覆盖能力和 final exact-rerank 结果

**Non-Goals:**
- 不把该 baseline 与 BoundFetch 的 overlap、CRC、preload、secondary assignment 等机制耦合
- 不在第一版实现在线服务、动态插入、后台 compaction 或复杂缓存层
- 不把 payload backend 重写进该工程；payload 继续通过外部 backend 层访问
- 不在第一版构造 full integrated DB engine，只做最简 baseline pipeline
- 不改 `RaBitQ-Library` 官方 search kernel 本身
- 不在这次修改中重做 coarse clustering，只修正评测与导出 contract

## Decisions

### 1. 工程独立落在 `third_party`，而不是主仓库内部模块

选择：
- 在 `/home/zcq/VDB/third_party` 下落一个独立 baseline 工程，并通过 CLI 与 formal-study 对接

原因：
- 避免 baseline 随主仓库优化一起漂移
- 更符合“官方方法 + 最小 glue code”的定位
- 便于后续冻结 commit / version 作为论文 baseline 依据

备选方案：
- 直接在主仓库 `src/` 下加一个 stripped-down runner
- 否决原因：容易复用到主方法特有优化，边界不清

### 2. 存储布局采用三层分离，而不是把 raw vector 塞进 `.clu`

选择：
- compressed index plane:
  - `meta.json`
  - `coarse_centroids.bin`
  - `cluster_offsets.bin`
  - `clusters.clu`
- raw vector plane:
  - `vectors.bin`
- payload plane:
  - 继续复用外部 `FlatStor / Lance / Parquet`

原因：
- 与当前 `IVF+PQ+rerank` baseline 的协议更对齐
- 便于分别度量 compressed scan、raw rerank 和 payload fetch
- 减少第一版索引格式复杂度

备选方案：
- 在 `clusters.clu` 中直接嵌入 raw vector 或 raw address
- 否决原因：会增加 block 复杂度，并过早把 baseline 变成系统优化工程

### 3. raw vector plane 采用按 `row_id` 定长寻址的扁平文件

选择：
- `vectors.bin` 以 `row_id` 顺序存储 `float32[dim]`
- 偏移由 `row_id * dim * sizeof(float)` 直接计算

原因：
- 查询时 exact rerank 最简单
- 便于 `pread`、mmap 和后续 profile
- 不依赖复杂 schema 或额外索引

备选方案：
- 使用 `.npy` / `.fvecs`
- 否决原因：虽然可行，但作为独立 baseline 工程时，统一扁平二进制更稳且更易跨语言复用

### 4. `clusters.clu` 只存 row id、RaBitQ code 和最小 side stats

选择：
- 每个 cluster block 最小字段为：
  - `cluster_id`
  - `num_records`
  - `row_ids[num_records]`
  - `rq_codes[num_records][code_size]`
  - `aux_stats`（仅保留官方 RaBitQ scoring 必需字段）

原因：
- 保持 compressed plane 纯粹，只承担 candidate generation
- 降低格式演化成本
- 便于针对 cluster block 单独做热路径 profile

备选方案：
- 一开始就加入 raw-vector address、payload pointer、预取 hint
- 否决原因：会让第一版变成面向 serving 的综合索引，而不是最简 baseline

### 5. coarse clustering 和 probing 直接依赖 Faiss 语义

选择：
- build 阶段使用 Faiss 完成 coarse centroid 训练和向量分配
- search 阶段使用 Faiss 风格参数：
  - `nlist`
  - `nprobe`
  - metric

原因：
- 保证与现有 IVF baseline 口径一致
- 用户已经要求聚类和搜索直接复用或参考 Faiss
- 后续 formal-study 横向比较时参数语义更统一

备选方案：
- 自行实现 coarse quantizer
- 否决原因：没有必要，且会引入额外实现偏差

### 6. 查询协议固定为 “official IVF index search -> exact rerank -> payload fetch”

选择：
- build CLI 只负责生成 compressed plane 和 raw vector plane
- search CLI 负责：
  - load official IVF-RaBitQ index
  - run official compressed candidate search
  - candidate budget 管理
  - exact rerank
  - 导出 top-k ids / metrics
- payload fetch 不内嵌到核心检索 CLI，而是保留可选 hooks 或外部 wrapper

原因：
- 这样可以既支持 search-only benchmark，也能被 formal-study 的 coupled E2E 包装层复用
- 避免在第一版把 payload backend 接口复杂化
- 同时避免第一版自研 `.clu` 直读查询核，降低实现风险

备选方案：
- 直接从 `cluster_offsets.bin + clusters.clu` 实现完整 query kernel
- 否决原因：会把第一版变成自定义搜索引擎，而不是官方 baseline 的最简包装

### 7. CLI 参数直接面向实验方案，不隐藏 Faiss 核心控制轴

选择：
- build CLI 最少暴露：
  - `--base`
  - `--output`
  - `--dim`
  - `--metric`
  - `--nlist`
  - `--train-size`
  - `--rabitq-*`
- search CLI 最少暴露：
  - `--index-dir`
  - `--queries`
  - `--raw-vectors`
  - `--nprobe`
  - `--candidate-budget`
  - `--topk`
  - `--output`

原因：
- 当前 formal-study 的 sweep 就围绕 `nlist / nprobe / topk / candidate_budget`
- 参数直暴露比隐藏在 config file 更适合自动化 sweep

备选方案：
- 仅使用单个 config 文件
- 否决原因：可读性和实验脚本可组合性较差

### 8. `recall@k` 必须采用标准 overlap 定义，hit-style 指标降级为辅助指标

选择：
- `recall@k` 改为：
  - `pred = set(pred_ids[:k])`
  - `gt = set(gt_ids[:k])`
  - `recall@k(query) = |pred ∩ gt| / k`
  - 对 query 取平均
- 现有 hit-style 指标改为单独字段，例如：
  - `hit_rate@10`
  - `hit_rate@20`

原因：
- 这与主仓库 `bench_e2e`、当前 BoundFetch 分析文档和 Faiss/DiskANN 对照的定义一致
- 只有这样，`IVF+RQ+rerank` 的结果才能与现有 warm / formal-study 结果直接比较
- hit-style 指标仍有诊断意义，但它表达的是“是否至少命中一个真邻居”，不是 recall

备选方案：
- 保持当前 hit-style 结果不变，只在文档里解释
- 否决原因：结果文件里继续写 `recall@10` 会持续污染后续聚合和图表

### 9. 将 coarse candidate 覆盖与 final rerank 结果分开导出

选择：
- 在 `ivf_rabitq_rerank.py` 中增加两类指标：
  - `candidate_recall@k`
  - `final_recall@k`
- 其中：
  - `candidate_recall@k(query) = |candidate_set ∩ gt_topk| / k`
  - `final_recall@k(query) = |pred_topk ∩ gt_topk| / k`
- `metrics.csv` 中的主字段 `recall@10`、`recall@20` 指向 `final_recall`

原因：
- 这样才能回答“是 coarse clustering 好，还是 candidate budget + exact rerank 把结果补上来的”
- 对 `IVF+RQ+rerank` 这种两阶段 pipeline，这比只看 final top-k 更有解释力

备选方案：
- 只修 final recall，不新增 candidate 指标
- 否决原因：仍然很难解释 `nprobe=64` 等点到底强在哪里

### 10. 历史结果必须显式隔离，避免旧口径混入新图表

选择：
- 修改后的 runner 在 `run_meta.json` 或 `metrics.csv` 中显式写入：
  - `recall_definition=overlap`
  - `candidate_recall_definition=overlap`
  - `hit_rate_definition=at_least_one_hit`
- 聚合脚本只读取 `recall_definition=overlap` 的结果进入正式 summary

原因：
- 旧结果已经存在于 `baselines/formal-study/outputs/.../ivf_rabitq_rerank/`
- 如果不加口径标识，后续聚合时会把旧的 hit-style `recall@10` 和新的标准 recall 混在一起

备选方案：
- 直接覆盖旧结果目录
- 否决原因：会失去审计链条，也不利于解释为什么数字发生了变化

## Risks / Trade-offs

- [RaBitQ-Library 与自定义 sidecar 数据布局不完全兼容] → 以官方 `official.ivf_rabitq.index` 作为查询主索引，`.clu` 只由 build 期对象直接导出，不从官方索引逆向解析
- [third_party 工程与 formal-study 脚本集成需要额外 wrapper] → 第一版明确以 CLI 和稳定 metrics 输出为接口，后续仅补 wrapper，不回改索引格式
- [raw vector 扁平文件在大数据集上随机读放大明显] → 作为最简 baseline 接受这一点，并把优化留到后续独立 change
- [用户后续仍想加入更复杂的 cluster-local raw vector 布局] → 当前格式中保留版本号和扩展字段，但第一版不实现
- [payload fetch 不内嵌可能被误解为不是完整 E2E] → 在 design 和 spec 中明确：核心 baseline 提供 search/rerank 协议，coupled E2E 由 formal-study wrapper 衔接
- [历史 hit-style `recall@10` 已经写入结果目录] → 增加 `recall_definition` 元数据并要求正式聚合时过滤旧口径
- [candidate recall 需要 candidate ids 暴露] → 允许 `search_core` 或 Python wrapper 额外导出 pre-rerank candidate ids，不改变最终 top-k 文件格式

## Migration Plan

1. 在 `/home/zcq/VDB/third_party` 下创建独立工程目录，并固定 external dependency 入口为官方 `RaBitQ-Library` 和本机 Faiss
2. 先实现 build path：
   - 读 base vectors
   - 训练 coarse centroids
   - cluster assignment
   - 生成 `clusters.clu`、`cluster_offsets.bin`、`coarse_centroids.bin`、`meta.json`
   - 导出 `vectors.bin`
3. 再实现 search path：
   - 读 query
   - 加载官方 `official.ivf_rabitq.index`
   - 用官方 IVF search 生成 `candidate_budget`
   - exact rerank on `vectors.bin`
   - 导出 ids 和 metrics
4. 提供最小 smoke benchmark，先在 `coco_100k` 上验证 build/search 正确性
5. 再补 formal-study wrapper，使该 baseline 能进入现有 experiment plan
6. 若需要回滚，保持该 baseline 工程自包含，删除对应 wrapper 即可，不影响主仓库搜索路径
7. 修正 formal-study wrapper 的 recall 定义，并为旧结果增加口径隔离标记
8. 重新生成 `IVF+RQ+rerank` 在 `coco_100k` 上的正式 summary，再与 BoundFetch / Faiss / DiskANN 做同口径比较

## Open Questions

- `RaBitQ-Library` 官方接口是否要求额外 side-statistics 持久化到每条记录，还是可以只在 build 时生成固定 cluster-level 参数？
- 第一版 search CLI 是否需要直接输出 recall-ready top ids，还是只输出 final ids 让上层脚本统一评测？
- formal-study 集成时，payload fetch 应该由单独 Python wrapper 驱动，还是补一个可选 `--payload-backend` 的二进制参数？
- candidate ids 最稳妥的导出位置是 `search_core` 额外写 `candidate_ids.npy/bin`，还是继续在 Python wrapper 中通过已有输出重构？
