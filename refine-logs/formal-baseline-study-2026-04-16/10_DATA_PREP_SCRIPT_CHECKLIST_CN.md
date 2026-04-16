# 数据准备脚本清单

日期：2026-04-16

## 1. 目的

本文件把 `08_DOWNLOAD_AND_PREP_CHECKLIST_CN.md` 进一步细化到脚本级别。

目标不是立即实现所有脚本，而是先把：

- 脚本名称
- 输入路径
- 输出路径
- 依赖
- 失败检查点

全部冻结下来。

## 2. 路径约定

### 原始数据与原始 embedding

- 根目录：`/home/zcq/VDB/data`
- 正式 baseline 数据建议放在：
  - `/home/zcq/VDB/data/formal_baselines`
- 这里仅保存：
  - 原始下载数据
  - 原始 embedding

### baseline 格式化数据

- 根目录：`/home/zcq/VDB/baselines/data`
- 正式 baseline/backend 导出建议放在：
  - `/home/zcq/VDB/baselines/data/formal_baselines`
- 这里保存：
  - 清洗后的 canonical 数据
  - split manifest 与 ground truth
  - FlatStor / Lance / Parquet 等格式化数据
  - 为各个 baseline 定制的 backend 写入结果

### baseline 运行产物

- 根目录：`/home/zcq/VDB/baselines`
- 正式 baseline 运行建议放在：
  - `/home/zcq/VDB/baselines/formal-study`

### 第三方库

- 根目录：`/home/zcq/VDB/third_party`

## 3. 建议脚本目录

```text
/home/zcq/VDB/baselines/formal-study/scripts/
├── prepare_datasets/
├── build_embeddings/
├── build_groundtruth/
├── export_payload_backends/
└── sanity_checks/
```

## 4. 主数据集脚本清单

### 4.1 COCO 100K

#### `prepare_datasets/coco_100k_manifest.py`

用途：

- 检查本地 COCO 100K 资产是否齐全
- 生成统一 manifest

输入：

- `/home/zcq/VDB/data/formal_baselines/coco_100k/`

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/cleaned/payload.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/splits/split_v1.json`

检查点：

- `image_embeddings.npy`
- `query_embeddings.npy`
- 图片路径可解析

#### `build_groundtruth/coco_100k_gt.py`

用途：

- 生成 `gt_top10.npy` 和 `gt_top20.npy`

输入：

- `/home/zcq/VDB/data/formal_baselines/coco_100k/embeddings/image_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/coco_100k/embeddings/query_embeddings.npy`

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/gt/gt_top20.npy`

#### `export_payload_backends/coco_100k_payload_export.py`

用途：

- 将 COCO payload 导出到 FlatStor / Lance / Parquet

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/payload_flatstor/`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/payload_lance/`
- `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/payload_parquet/`

### 4.2 MS MARCO Passage

#### `prepare_datasets/msmarco_download.sh`

用途：

- 下载 passages / queries / qrels

输入：

- 官方下载链接

输出：

- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/raw/`

检查点：

- passage 文件完整
- query 文件完整
- qrels 文件完整

#### `prepare_datasets/msmarco_prepare.py`

用途：

- 清洗 passages / queries
- 构造 parquet
- 固定 split

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/cleaned/passages.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/cleaned/queries.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/splits/split_v1.json`

#### `build_embeddings/msmarco_embed.py`

用途：

- 用冻结的文本 encoder 生成 embeddings

输出：

- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/passage_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/query_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/embedding_meta.json`

#### `build_groundtruth/msmarco_gt.py`

用途：

- 生成 ANN gt
- 保留 qrels 映射

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top20.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/qrels.tsv`

#### `export_payload_backends/msmarco_payload_export.py`

用途：

- 导出 passage payload 到各 backend

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_flatstor/`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_lance/`
- `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_parquet/`

### 4.3 Amazon ESCI

#### `prepare_datasets/esci_download.sh`

用途：

- 拉取官方 ESCI 数据

输出：

- `/home/zcq/VDB/data/formal_baselines/amazon_esci/raw/`

#### `prepare_datasets/esci_prepare.py`

用途：

- 合并 products / queries / labels
- 固定 locale
- 构造商品文本

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/cleaned/products.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/cleaned/queries.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/cleaned/labels.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/splits/split_v1.json`

关键字段拼接规则：

- `title + brand + bullet_point + description`

#### `build_embeddings/esci_embed.py`

用途：

- 生成 query / product embeddings

输出：

- `/home/zcq/VDB/data/formal_baselines/amazon_esci/embeddings/product_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/amazon_esci/embeddings/query_embeddings.npy`
- `/home/zcq/VDB/data/formal_baselines/amazon_esci/embeddings/embedding_meta.json`

#### `build_groundtruth/esci_gt.py`

用途：

- 生成 ANN gt
- 保留 ESCI labels

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/gt_top20.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/esci_labels.parquet`

#### `export_payload_backends/esci_payload_export.py`

用途：

- 导出 ESCI payload 到各 backend

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_flatstor/`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_lance/`
- `/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_parquet/`

### 4.4 Deep8M + synthetic payload

#### `prepare_datasets/deep8m_link_or_copy.sh`

用途：

- 将现有 benchmark 数据链接或复制到 formal baseline 工作区

输入：

- 本地已有 `deep8m` 资产

输出：

- `/home/zcq/VDB/data/formal_baselines/deep8m_synth/raw/base.fvecs`
- `/home/zcq/VDB/data/formal_baselines/deep8m_synth/raw/query.fvecs`
- `/home/zcq/VDB/data/formal_baselines/deep8m_synth/raw/groundtruth.ivecs`

#### `prepare_datasets/deep8m_generate_synth_payload.py`

用途：

- 生成 deterministic synthetic payload

参数：

- `--payload-size 256`
- `--payload-size 4096`
- `--payload-size 65536`

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/cleaned/payload_256B.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/cleaned/payload_4KB.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/cleaned/payload_64KB.parquet`

#### `build_groundtruth/deep8m_convert_gt.py`

用途：

- 将原始 gt 转换为统一 `npy` 口径

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/gt/gt_top10.npy`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/gt/gt_top20.npy`

#### `export_payload_backends/deep8m_payload_export.py`

用途：

- 将三档 synthetic payload 导出到各 backend

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_256B_flatstor/`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_256B_lance/`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_256B_parquet/`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_4KB_*`
- `/home/zcq/VDB/baselines/data/formal_baselines/deep8m_synth/payload_64KB_*`

## 5. 扩展数据集脚本清单

### 5.1 LAION subset

#### `prepare_datasets/laion_subset_sample.py`

用途：

- 依据固定规则抽样 LAION 子集

输出：

- `/home/zcq/VDB/baselines/data/formal_baselines/optional/laion_subset/splits/sample_manifest.parquet`
- `/home/zcq/VDB/baselines/data/formal_baselines/optional/laion_subset/cleaned/payload.parquet`

要求：

- 保存抽样 seed
- 保存过滤规则

### 5.2 Clotho

#### `prepare_datasets/clotho_download_manual.md`

用途：

- 记录下载入口和手动整理步骤

### 5.3 MSR-VTT

#### `prepare_datasets/msrvtt_download_manual.md`

用途：

- 记录下载入口和手动整理步骤

## 6. 第三方库准备脚本清单

### 6.1 Faiss

目录：

- `/home/zcq/VDB/third_party/faiss`

建议文件：

- `README.local.md`
- `build.sh`
- `commit.txt`

### 6.2 DiskANN

目录：

- `/home/zcq/VDB/third_party/diskann`

建议文件：

- `README.local.md`
- `build.sh`
- `commit.txt`

### 6.3 Extended-RaBitQ

目录：

- `/home/zcq/VDB/third_party/extended-rabitq`

建议文件：

- `README.local.md`
- `build.sh`
- `commit.txt`

### 6.4 ConANN

目录：

- `/home/zcq/VDB/third_party/conann`

建议文件：

- `README.local.md`
- `build.sh`
- `commit.txt`

## 7. 每个脚本都必须回答的问题

在正式实现脚本前，每个脚本都要先补齐这 6 个字段：

1. 输入路径是什么
2. 输出路径是什么
3. 依赖哪些第三方库
4. 失败时怎么重试
5. 成功的检查点是什么
6. 运行日志写到哪里

## 8. 第一批最值得先写的脚本

建议优先实现：

1. `coco_100k_manifest.py`
2. `msmarco_download.sh`
3. `msmarco_prepare.py`
4. `msmarco_embed.py`
5. `deep8m_generate_synth_payload.py`
6. `esci_prepare.py`

原因：

- 这 6 个脚本覆盖了你主线里最关键的三个数据源：现有图文、真实文本、合成 payload。
