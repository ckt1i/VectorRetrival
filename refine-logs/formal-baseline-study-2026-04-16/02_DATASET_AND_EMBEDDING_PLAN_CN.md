# 数据集与嵌入计划

日期：2026-04-16

## 1. 数据集分层

正式实验数据集分成三层。

### A. 主实验数据集

| 数据集 | 类型 | 角色 | 是否进主表 |
|--------|------|------|------------|
| COCO 100K | 图文多模态 | 当前 E2E 主场景 | 是 |
| MS MARCO Passage | 文本检索 | 真实文本检索主 benchmark | 是 |
| Amazon ESCI | 商品检索 | 真实 product search benchmark | 是 |
| Deep8M + synthetic payload | 标准向量 + 合成 payload | 分离 ANN 核和 payload 大小效应 | 是 |

### B. 扩展实验数据集

| 数据集 | 类型 | 角色 | 是否进主表 |
|--------|------|------|------------|
| Deep1M | 标准向量 | 回归检查、小规模 sanity check | 否 |
| Amazon Reviews 2023 | 文本/商品元数据 | payload-rich catalog 扩展 | 否 |
| LAION subset | 图文多模态 | 大规模图文扩展 | 否 |

### C. 可选多模态附录

| 数据集 | 类型 | 角色 | 是否进主表 |
|--------|------|------|------------|
| Clotho | 音频文本 | 音频检索附录 | 否 |
| MSR-VTT | 视频文本 | 视频检索附录 | 否 |

## 2. 各数据集的 payload 设计

### COCO 100K

- 向量：
  - 图像 embedding 作为 database
  - 文本 embedding 作为 query 或补充 query
- 原始数据：
  - 图片文件
  - caption
  - image id
  - 基础 metadata
- 作用：
  - 检验 BoundFetch 在图文 payload 协同读取下的 E2E 表现

### MS MARCO Passage

- 向量：
  - passage embedding
  - query embedding
- 原始数据：
  - passage text
  - pid
  - title 或 source 字段，如果存在则一并保留
- 作用：
  - 最标准的真实文本 dense retrieval benchmark
- 附加指标：
  - `MRR@10`
  - `nDCG@10`

### Amazon ESCI

- 向量：
  - query embedding
  - product text embedding，建议使用 `title + bullet + description + brand`
- 原始数据：
  - product title
  - brand
  - bullet points
  - description
  - locale / product id
- 作用：
  - 真实商品检索 benchmark
- 附加指标：
  - `nDCG`
  - ESCI relevance 分级统计

### Deep8M + synthetic payload

- 向量：
  - 标准 base / query 向量
- 原始数据：
  - 合成 payload
- 合成 payload 档位：
  - `256B`
  - `4KB`
  - `64KB`
- 作用：
  - 把 payload 大小效应从真实语义中拆开

## 3. 嵌入生成原则

### 总原则

- 同一个数据集内，BoundFetch 与全部 baseline 必须共享同一套 embedding。
- 不比较 encoder 本身。
- encoder 的所有配置必须记录：
  - 模型名
  - 版本
  - pooling 方式
  - 归一化方式
  - batch size
  - 精度

### 推荐 encoder 方案

| 场景 | 推荐 encoder | 备注 |
|------|--------------|------|
| 英文文本检索 | BGE/E5 类双塔模型 | 稳定、易复现 |
| 图文检索 | CLIP 或 SigLIP | COCO 和 LAION 子集统一 |
| 音频文本 | CLAP | 只用于附录 |
| 视频文本 | 图像 encoder 做帧池化的轻量方案 | 先做低工程成本版本 |

### 归一化规则

- 若采用 cosine / inner product，必须统一向量归一化。
- 若采用 L2，必须记录是否做了 unit norm。
- ground truth 的生成方式必须与检索 metric 完全一致。

## 4. 目录组织建议

建议在数据目录下为正式 baseline 单独建立统一结构：

```text
/home/zcq/VDB/data/formal_baselines/
├── coco_100k/
│   ├── raw/
│   ├── embeddings/
│   ├── gt/
│   ├── payload_flatstor/
│   ├── payload_lance/
│   └── payload_parquet/
├── msmarco_passage/
├── amazon_esci/
├── deep8m_synth/
└── optional/
    ├── laion_subset/
    ├── clotho/
    └── msrvtt/
```

## 5. 合成 payload 生成规则

必须 deterministic。

推荐规则：

- 用 `doc_id` 作为随机种子
- 固定字段模板
- 固定长度档位
- 每次重跑得到完全相同的 payload 内容和大小

示例字段：

- `doc_id`
- `title`
- `body`
- `tags`
- `blob_padding`

## 6. ground truth 规则

### ANN ground truth

- 全部主数据集都需要 exact top-k ground truth
- 输出统一保存为：
  - `gt_top10.npy`
  - `gt_top100.npy`

### 任务标签 ground truth

- MS MARCO：
  - 官方 qrels
- ESCI：
  - 官方 relevance labels

## 7. 主实验优先级

推荐执行顺序：

1. COCO 100K
2. MS MARCO Passage
3. Deep8M + synthetic payload
4. Amazon ESCI
5. LAION subset
6. Clotho / MSR-VTT

这个顺序的原因：

- COCO 已有现成资产，最容易对接现有系统。
- MS MARCO 最适合做真实文本主 benchmark。
- Deep8M 最适合隔离 payload 大小效应。
- ESCI 的业务相关性高，但清洗成本高于 MS MARCO。
