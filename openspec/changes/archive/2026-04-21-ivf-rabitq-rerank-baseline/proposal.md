## Why

当前 formal-study 里已经有 `IVF+PQ+rerank` 和 `DiskANN` 方向，但缺少一条基于官方 `RaBitQ-Library` 的、结构清晰且可独立复现的 `IVF+RQ+exact rerank` baseline。继续直接复用主仓库实现会把 baseline 与 BoundFetch 特有优化耦合在一起，不利于形成可解释、可冻结、可接入现有实验计划的对照线。

这个 baseline 当前还有一个评测口径问题：formal-study wrapper 里的 `recall@10` 实际被实现成了“预测 top-k 和 GT top-k 只要有任意交集就记 1”的 hit-style 成功率，而不是主仓库 `bench_e2e`、Faiss/DiskANN 对照和分析文档一直使用的标准 top-k overlap recall。这个差异会显著抬高数值，导致 `IVF+RQ+rerank` 与其他方法不能公平比较，也会错误放大 `nprobe=64` 等配置下 coarse clustering 看起来的效果。

## What Changes

- 在 `/home/zcq/VDB/third_party` 下新增一个独立的 `IVF + RaBitQ + exact rerank` baseline 工程，作为与主仓库解耦的最简实现。
- 新增基于 Faiss 的 coarse clustering / probing 组件，要求复用或参考 Faiss 的：
  - coarse quantizer 训练
  - `nlist` 聚类划分
  - `nprobe` 搜索流程
  - metric 选择与 top-k 候选管理
- 新增一套磁盘存储布局，明确分离：
  - compressed index plane：`coarse centroids + cluster offsets + cluster blocks(.clu)`
  - raw vector plane：按 `row_id` 定长寻址的 `vectors.bin`
  - payload plane：继续复用外部 `FlatStor / Lance / Parquet`
- 新增 index build CLI，要求能够指定：
  - 输入向量路径
  - 输出目录
  - `nlist`
  - metric
  - Faiss 训练样本规模
  - RaBitQ 相关配置
- 新增 search / rerank CLI，要求能够指定：
  - index 输出路径
  - query 向量路径
  - `nprobe`
  - `candidate_budget`
  - `topk`
  - raw vector 路径
  - payload backend 参数
- 新增 formal-study 兼容输出要求，使该 baseline 可以被当前实验计划消费，包括统一的 metrics 导出和 run metadata。
- 修改 formal-study wrapper 的指标 contract，使 `recall@10` 改为标准定义：
  - `recall@k = |pred_topk ∩ gt_topk| / k`
  - 再对所有 query 取平均
- 将现有 hit-style 指标显式改名为 `hit_rate@k` 或等价诊断字段，禁止继续写入 `recall@10`
- 新增 candidate-level 诊断输出，区分：
  - rerank 前候选集合对 GT top-k 的覆盖率
  - rerank 后最终 top-k 的标准 recall
- 明确第一版只实现最简 baseline：
  - 不复用 BoundFetch 的 overlap / CRC / preload / early-stop 机制
  - 不把 raw vector 混入 `.clu`
  - 不在第一版实现复杂后台服务或在线更新

## Capabilities

### New Capabilities
- `ivf-rabitq-baseline-storage`: 定义 `IVF + RaBitQ + rerank` baseline 的磁盘布局、元数据格式和 compressed/raw/payload 三层分离约束
- `ivf-rabitq-baseline-build`: 定义基于 Faiss coarse quantizer 和官方 RaBitQ 库的建索引流程、参数接口和输出文件要求
- `ivf-rabitq-baseline-query`: 定义基于 Faiss probing、RaBitQ cluster scan、raw-vector exact rerank 和统一 CLI/metrics 输出的查询协议

### Modified Capabilities
- `ivf-rabitq-baseline-query`: 查询与 formal-study wrapper 的指标输出从 hit-style 成功率改成标准 top-k overlap recall，并额外导出 hit-rate / candidate recall 作为诊断字段

## Impact

- Affected implementation roots:
  - `/home/zcq/VDB/third_party/` 下新增 baseline 工程目录
  - `/home/zcq/VDB/VectorRetrival/openspec/changes/ivf-rabitq-rerank-baseline/`
- Affected external dependencies:
  - 官方 `RaBitQ-Library`
  - Faiss（用于 coarse quantizer、clustering、probing 参考实现）
- Affected experiment integration:
  - formal-study 后续可新增 `IVF-RaBitQ-rerank` baseline runner
  - 输出需要能适配现有 warm / formal-study 实验方案的参数和结果记录口径
  - `recall@10` 的语义必须与主仓库一致，不能再使用 hit-style 指标冒充 recall
- Affected storage outputs:
  - `meta.json`
  - `coarse_centroids.bin`
  - `cluster_offsets.bin`
  - `clusters.clu`
  - `vectors.bin`
