# 执行与记录手册

日期：2026-04-16

## 1. 执行目标

把正式 baseline 方案变成一套可重复执行的实验流水线，而不是停留在讨论层。

## 2. 建议的执行阶段

### Phase 0：冻结清单

必须先冻结三份清单：

1. 数据集清单
2. encoder 清单
3. baseline 方法清单

冻结后不再临时改模型或改 split。

## 3. 推荐目录

路径口径统一如下：

- `/home/zcq/VDB/data`
  - 原始数据
  - 原始 embedding
- `/home/zcq/VDB/baselines/data`
  - 清洗后的 canonical 数据
  - split manifest 与 ground truth
  - baseline/backend 导出的格式化数据
- `/home/zcq/VDB/baselines`
  - 实验运行产物、CSV、图表、tracker

baseline 运行产物统一放在 `/home/zcq/VDB/baselines` 下。

建议在该路径下采用如下结构：

```text
/home/zcq/VDB/baselines/formal-study/
├── manifests/
│   ├── dataset_registry.csv
│   ├── encoder_registry.csv
│   └── baseline_registry.csv
├── scripts/
│   ├── prepare_datasets/
│   ├── build_embeddings/
│   ├── run_vector_search/
│   ├── run_payload_microbench/
│   └── run_e2e_coupled/
├── outputs/
│   ├── vector_search/
│   ├── payload_microbench/
│   ├── e2e/
│   ├── build_cost/
│   └── plots/
└── trackers/
    ├── RUNBOOK_CN.md
    ├── RUN_STATUS.csv
    └── FAILURES.md
```

baseline/backend 写出的格式化数据建议统一放在：

```text
/home/zcq/VDB/baselines/data/formal_baselines/
├── coco_100k/
├── msmarco_passage/
├── amazon_esci/
├── deep8m_synth/
└── optional/
```

如果某个 baseline 依赖手动下载或本地编译的第三方库，统一放在：

```text
/home/zcq/VDB/third_party/
├── faiss/
├── diskann/
├── extended-rabitq/
├── conann/
└── other/
```

## 4. 每个阶段的具体动作

## 4.0 主协议修订

从本版本开始，正式 E2E 主协议不再采用“向量搜索先跑完、payload 再离线读取、最后合并”的方式。

正式口径改为：

- 对每个 query，只提交一次查询请求。
- 向量搜索在产生最终需要读取的结果后，立即触发原始数据读取。
- 计时起点是“提交查询向量进入系统”。
- 计时终点是“该 query 对应原始数据读取完成”。

因此：

- `e2e_ms` 表示单次查询从进入系统到完成原始数据读取的完整时延。
- `ann_core_ms` 只作为分解指标保留，不再单独充当主结果。
- “搜索结果与 payload 读取分头跑再合并”只允许作为诊断或 backend 微基准，不进入主表。

### Phase 1：准备数据

对每个主数据集执行：

1. 下载原始数据
2. 生成统一 split
3. 清洗并导出 payload
4. 生成统一 manifest

输出：

- `dataset_registry.csv`
- 每个数据集的 `README.md`

### Phase 2：生成嵌入

对每个主数据集执行：

1. 运行固定 encoder
2. 导出 database embeddings
3. 导出 query embeddings
4. 写入 encoder manifest

输出：

- `base.npy` / `query.npy`
- `embedding_meta.json`

### Phase 3：生成 ground truth

对每个数据集执行：

1. 计算 exact ANN top-k
2. 若有任务标签，额外写入 qrels / relevance labels 映射

输出：

- `gt_top10.npy`
- `gt_top20.npy`
- `task_labels.parquet` 或原始 qrels 映射

### Phase 4：跑搜索核 baseline

搜索核按固定顺序执行：

1. BoundFetch
2. IVFPQ + rerank
3. IVFPQR
4. IVF-RQ
5. DiskANN
6. ConANN 可选
7. 外部 RaBitQ 可选

每个方法都必须输出统一格式的 CSV。

### Phase 5：跑联动式 E2E baseline

正式 E2E 运行必须满足以下条件：

1. 向量搜索和原始数据读取在同一条 query 路径里完成。
2. 一旦系统判定某个 query 的最终结果集或最终需要 fetch 的候选，就立刻发起原始数据读取。
3. 统计时间覆盖：
   - query vector 进入系统
   - coarse / probe / rerank
   - payload fetch
   - 必要的 decode / assemble
4. 统计终点是该 query 对应原始数据全部读取完成。

每个系统都必须输出：

- `ann_core_ms`
- `payload_fetch_ms`
- `bytes_read`
- `fetch_count`
- `overlap_ratio`（若系统支持）
- `e2e_ms`

### Phase 6：跑 payload backend 微基准

payload backend 按固定顺序执行：

1. FlatStor
2. Lance
3. Parquet

这一阶段的定位是：

- 只用于拆解后端差异
- 只用于辅助解释 `payload_fetch_ms`
- 不用于主表替代正式 E2E

输入可以使用统一的 top-k ids 或预生成 id 列表。

### Phase 7：结果汇总

结果汇总时只做两件事：

1. 汇总正式 E2E 结果、搜索核结果、payload 微基准结果。
2. 生成统一表和统一图。

不允许在这个阶段重新用离线拼接方式替代正式 E2E 结果。

## 5. 输出文件规范

### 搜索核输出

建议统一为：

`outputs/vector_search/{dataset}/{system}/{run_id}/metrics.csv`

### payload 微基准输出

建议统一为：

`outputs/payload_microbench/{dataset}/{backend}/{run_id}/metrics.csv`

### 正式 E2E 输出

建议统一为：

`outputs/e2e/{dataset}/{system}/{run_id}/metrics.csv`

并额外生成：

`outputs/e2e/{dataset}/e2e_system_summary.csv`

### 成本输出

建议统一为：

`outputs/build_cost/build_startup.csv`

## 6. 必须记录的 tracker

### RUN_STATUS.csv

字段建议：

- `run_id`
- `phase`
- `dataset`
- `system`
- `backend`
- `config`
- `protocol`
- `status`
- `start_time`
- `end_time`
- `output_dir`
- `notes`

### FAILURES.md

每次失败都要记录：

- 失败时间
- 数据集
- 方法
- 配置
- 错误日志路径
- 是否重试
- 最终处理方式

## 7. 推荐运行顺序

### 第一批

1. COCO 100K
2. IVFPQ + rerank
3. IVFPQR
4. BoundFetch
5. DiskANN 参考
6. FlatStor / Lance 联动式 E2E

目标：

- 先把你最熟悉的数据和“联动式 E2E 主协议”固定下来

### 第二批

1. MS MARCO Passage
2. BoundFetch
3. IVFPQ + rerank
4. IVFPQR
5. IVF-RQ
6. 主后端联动式 E2E

目标：

- 拿下真实文本主 benchmark

### 第三批

1. Deep8M + synthetic payload
2. 三种 payload 大小档位
3. 联动式 E2E
4. storage backend 微基准

目标：

- 拆分 payload 大小效应和存储后端效应

### 第四批

1. Amazon ESCI
2. LAION subset
3. Clotho / MSR-VTT

目标：

- 补齐更强应用场景的 appendix

## 8. 当前建议的最小交付集

如果现在只做一轮最关键的正式实验，最小交付集是：

1. COCO 100K
2. MS MARCO Passage
3. Deep8M + synthetic payload
4. BoundFetch
5. IVFPQ + rerank
6. IVFPQR
7. IVF-RQ
8. DiskANN
9. FlatStor
10. Lance

Parquet、LAION、音频、视频都可以后置。

## 9. 正式 E2E 的记录原则

正式 E2E 表必须满足以下记录要求：

1. 每个 query 都要有单独时延样本。
2. 需要保留 `ann_core_ms` 与 `payload_fetch_ms`，但主排序字段是 `e2e_ms`。
3. 若系统具备重叠执行能力，必须记录 `overlap_ratio` 或等价指标。
4. 若某个 baseline 无法做到联动式执行，需要明确标注为：
   - `offline_composed_reference`
   - 该结果只能放参考表，不能与正式 E2E 主表同列解释。
