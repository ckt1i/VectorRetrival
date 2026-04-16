# 下载与预处理清单

日期：2026-04-16

## 1. 用途

本清单用于把正式 baseline 方案落成具体的数据准备流水线，覆盖：

- 原始数据下载
- split 固定
- 向量嵌入生成
- 原始 payload 整理
- synthetic payload 生成
- ground truth 生成

本清单只定义流程和产物，不在这里实现脚本。

## 2. 统一目录

原始数据和原始 embedding 统一放到：

`/home/zcq/VDB/data`

建议在该路径下使用：

```text
/home/zcq/VDB/data/formal_baselines/
├── coco_100k/
├── msmarco_passage/
├── amazon_esci/
├── deep8m_synth/
└── optional/
    ├── laion_subset/
    ├── clotho/
    └── msrvtt/
```

每个数据集目录建议固定如下子目录：

```text
{dataset}/
├── raw/
├── embeddings/
└── logs/
```

baseline 运行输出统一写到：

`/home/zcq/VDB/baselines`

清洗后的 canonical 数据、split、ground truth 与各个 baseline/backend 写出的格式化存储数据统一写到：

`/home/zcq/VDB/baselines/data`

建议在该路径下使用：

```text
/home/zcq/VDB/baselines/data/formal_baselines/
├── coco_100k/
│   ├── cleaned/
│   ├── splits/
│   ├── gt/
│   ├── payload_flatstor/
│   ├── payload_lance/
│   └── payload_parquet/
├── msmarco_passage/
├── amazon_esci/
├── deep8m_synth/
└── optional/
```

如果需要手动下载或本地编译第三方库，统一放到：

`/home/zcq/VDB/third_party`

## 3. Phase A：主数据集优先级

按下面顺序准备：

1. `COCO 100K`
2. `MS MARCO Passage`
3. `Deep8M + synthetic payload`
4. `Amazon ESCI`
5. `LAION subset`
6. `Clotho`
7. `MSR-VTT`

原因：

- COCO 已有现成资产，最快打通联动式 E2E。
- MS MARCO 是最重要的真实文本 benchmark。
- Deep8M 最适合拆分 payload 大小效应。
- ESCI 的清洗和字段拼接成本高于前面三项。

## 4. 数据集逐项清单

### 4.1 COCO 100K

状态：

- 优先使用你本地已有资产。
- 若已有 `image_embeddings.npy / query_embeddings.npy`，直接进入整理阶段。

目标产物：

- `/home/zcq/VDB/data/formal_baselines/coco_100k/embeddings/image_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/coco_100k/embeddings/query_embeddings.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/gt/gt_top20.npy`

同时在 `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/` 下生成：

- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/cleaned/payload.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/splits/split_v1.json`

payload 字段建议：

- `image_id`
- `image_path`
- `caption`
- `width`
- `height`

检查项：

- 确认 query 与 database 的 embedding 维度一致。
- 确认 metric 与现有 BoundFetch 实验口径一致。
- 确认联动式 E2E 能读取到真实图片或对应 metadata。

### 4.2 MS MARCO Passage

官方来源：

- Dataset page: `https://microsoft.github.io/msmarco`

准备步骤：

1. 下载 passages、queries、qrels。
2. 固定一个可复现 split。
3. 在 `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/cleaned/` 下构造 canonical parquet。
4. 生成 query / passage embeddings。
5. 生成 exact ANN `gt_top10.npy` 和 `gt_top20.npy`。
6. 保留任务评价所需 qrels 映射。

目标产物：

- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/raw/`：原始 tsv 或官方压缩包
- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/passage_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/query_embeddings.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top20.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/qrels.tsv`

同时在 `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/` 下生成：

- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/cleaned/passages.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/cleaned/queries.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/splits/split_v1.json`

payload 字段建议：

- `pid`
- `passage_text`
- `title`（若无可留空）

注意：

- ANN ground truth 与任务 qrels 不是一回事，必须分开保存。
- 论文主结果建议同时保留 `recall@10/20` 和 `MRR@10 / nDCG@10`。

### 4.3 Deep8M + synthetic payload

来源：

- 优先使用本地已有 benchmark 向量资产。
- 不要求重新从外部镜像下载。

准备步骤：

1. 确认 base/query/gt 文件完整。
2. 构造 deterministic synthetic payload。
3. 生成三档 payload：
   - `256B`
   - `4KB`
   - `64KB`
4. 导出到三种 backend。

目标产物：

- `/home/zcq/VDB/data/formal_baselines/deep8m_synth/raw/base.fvecs`
- `/home/zcq/VDB/data/formal_baselines/deep8m_synth/raw/query.fvecs`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/gt/groundtruth.ivecs`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/gt/gt_top20.npy`

同时在 `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/` 下生成：

- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/cleaned/payload_256B.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/cleaned/payload_4KB.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/cleaned/payload_64KB.parquet`

synthetic payload 规则：

- 用 `doc_id` 作为随机种子
- 每档大小完全固定
- 每次重跑内容一致

推荐字段：

- `doc_id`
- `title`
- `body`
- `tags`
- `blob_padding`

### 4.4 Amazon ESCI

官方来源：

- GitHub: `https://github.com/amazon-science/esci-data`

准备步骤：

1. 下载 queries/examples/products。
2. 固定 locale 范围与 split。
3. 构造商品文本：
   - `title + brand + bullet + description`
4. 生成 query / product embeddings。
5. 生成 ANN ground truth。
6. 保留 ESCI labels。

目标产物：

- `/home/zcq/VDB/data/formal_baselines/amazon_esci/embeddings/product_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/amazon_esci/embeddings/query_embeddings.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/gt_top20.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/esci_labels.parquet`

同时在 `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/` 下生成：

- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/cleaned/products.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/cleaned/queries.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/cleaned/labels.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/splits/split_v1.json`

payload 字段建议：

- `product_id`
- `title`
- `brand`
- `bullet_point`
- `description`
- `locale`

注意：

- 要在 manifest 里写清楚相关性定义是否把 `Substitute` 计为 relevant。

## 5. 扩展数据集清单

### 5.1 LAION subset

官方来源：

- Project page: `https://laion.ai/blog/laion-400-open-dataset/`

建议策略：

- 不下载完整大集
- 只构造可控子集
- 统一保存子集抽样规则

最小要求：

- 记录采样条件
- 记录图文对数量
- 记录去重与过滤逻辑

### 5.2 Clotho

官方来源：

- Zenodo page: `https://zenodo.org/record/3490683`

建议策略：

- 仅作 appendix
- 保留音频路径、caption、split

### 5.3 MSR-VTT

官方来源：

- Paper page: `https://www.cv-foundation.org/openaccess/content_cvpr_2016/html/Xu_MSR-VTT_A_Large_CVPR_2016_paper.html`

建议策略：

- 仅作 appendix
- 先用轻量帧采样和图像 encoder 池化方案，不引入复杂视频编码模型

## 6. 嵌入生成清单

所有数据集共用以下检查项：

1. encoder 先写入 `07_ENCODER_REGISTRY_TEMPLATE.csv`
2. 固定模型版本
3. 固定 pooling
4. 固定 normalize 策略
5. 固定 batch size 和 precision
6. 固定 query / document instruction 模板

每次生成后必须落以下文件：

- `embedding_meta.json`
- `base_or_doc_embeddings.npy`
- `query_embeddings.npy`
- `embedding_log.txt`

## 7. backend 导出清单

每个数据集准备完 payload 后，统一导出：

- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/payload_flatstor/`
- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/payload_lance/`
- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/payload_parquet/`

要求：

- 三个 backend 的字段完全对齐
- 不允许在不同 backend 上使用不同字段集合
- 文件大小与条目数必须记录

## 8. ground truth 清单

每个主数据集至少生成：

- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/gt/gt_top20.npy`

若有任务标签，再额外生成：

- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/gt/qrels.tsv`
- `/home/zcq/VDB/baselines/data/formal_baselines/{dataset}/gt/labels.parquet`

要求：

- metric 必须与检索 metric 一致
- 生成脚本必须记录 CPU/GPU、线程数和精度设置

## 9. 交付检查

一个数据集只有在以下条件全部满足后，才算“可进入 baseline 运行”：

- 原始数据已落地
- split 已冻结
- payload 已清洗
- embeddings 已生成
- encoder 已登记
- gt_top10 / gt_top20 已生成
- backend 导出至少完成主后端

## 10. backend 导出路径约定

各数据集的格式化 backend 数据统一导出到 `/home/zcq/VDB/baselines/data/formal_baselines`，例如：

- COCO 100K
  - `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/payload_flatstor/`
  - `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/payload_lance/`
  - `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/payload_parquet/`
- MS MARCO Passage
  - `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_flatstor/`
  - `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_lance/`
  - `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_parquet/`
- Amazon ESCI
  - `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_flatstor/`
  - `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_lance/`
  - `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_parquet/`
- Deep8M synthetic payload
  - `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_256B_*`
  - `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_4KB_*`
  - `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_64KB_*`

## 11. 建议先补的实际文件

优先补齐这些工作文件：

1. `manifests/dataset_registry.csv`
2. `manifests/encoder_registry.csv`
3. `trackers/RUN_STATUS.csv`
4. `trackers/FAILURES.md`
5. `scripts/prepare_datasets/README.md`
6. `scripts/build_embeddings/README.md`

## 12. 需要手动下载或编译的第三方库放置规则

若正式 baseline 依赖手动处理的第三方库，统一放在：

```text
/home/zcq/VDB/third_party/
├── faiss/
├── diskann/
├── extended-rabitq/
├── conann/
└── other/
```

建议规则：

1. 每个库单独一个目录。
2. 每个目录至少包含：
   - `README.local.md`
   - `build/` 或官方构建输出目录
   - `commit.txt` 或版本记录
3. 所有手动 patch 都要在对应目录中记录。

推荐优先登记的库：

- `faiss`
- `diskann`
- `extended-rabitq`
- `conann`（若采用外部实现）

## 13. 参考来源

本清单使用的主要官方入口：

- MS MARCO: `https://microsoft.github.io/msmarco`
- Amazon ESCI: `https://github.com/amazon-science/esci-data`
- LAION: `https://laion.ai/blog/laion-400-open-dataset/`
- Clotho: `https://zenodo.org/record/3490683`
