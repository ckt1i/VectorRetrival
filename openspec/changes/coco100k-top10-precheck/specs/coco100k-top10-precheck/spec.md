## 新增需求

### 需求：前置验证必须只在 `coco_100k` 上执行真实 baseline
前置验证阶段必须只使用 `coco_100k` 作为测试数据集，并且 baseline 必须来自真实第三方实现，而不是仓库内的替代路径。

#### 场景：只跑单一数据集且 baseline 来源正确
- **当** `coco100k-top10-precheck` 被执行
- **则** 只能在 `coco_100k` 上运行
- **且** `IVF + PQ` 必须由 Faiss `IndexIVFPQ` 实现
- **且** `IVF + RaBitQ` 必须由 `RaBitQ-Library` IVF 实现

### 需求：前置验证必须固定 `top10` 和 `top-200` 候选扩展
前置验证必须只执行 `top10`，并且所有 baseline 必须先产生 `top-(20 * topk)=200` 的候选集，再进入统一 rerank。

#### 场景：候选扩展一致
- **当** baseline 运行时
- **则** `topk` 必须为 `10`
- **且** 每次查询必须先得到 `200` 个候选 id
- **且** 最终 top-10 必须来自统一 rerank 后的结果

### 需求：前置验证必须复用固定的 2048 聚类文件
前置验证必须复用现成的 `coco_100k` 聚类文件，不允许在执行时默默切换到其他聚类输入。

#### 场景：聚类输入固定
- **当** baseline 构建或查询运行
- **则** 必须复用 `/home/zcq/VDB/data/coco_100k/coco100k_centroid_2048.fvecs`
- **且** 必须复用 `/home/zcq/VDB/data/coco_100k/coco100k_cluster_id_2048.ivecs`
- **且** `nlist` 必须为 `2048`
- **且** `nprobe` 必须为 `64`

### 需求：原始向量 rerank 与原始数据读取必须使用共享后端
两个 baseline 的向量检索 core 之后，必须统一进入共享原始向量 rerank 和共享 payload 读取后端。

#### 场景：后处理路径统一
- **当** 任一 baseline 输出候选结果
- **则** 必须从共享 `raw-vector store` 读取对应原始向量进行精确重排
- **且** 必须从共享 `payload store` 读取 final top-10 的原始数据
- **且** 不得将当前仓库的 `data.dat / cluster.clu` 视为本需求中的共享后端

### 需求：baseline 实现与执行脚本必须统一落在 `/home/zcq/VDB/baselines`
前置验证涉及的 baseline driver、构建脚本和执行脚本必须统一放在 `/home/zcq/VDB/baselines` 下。

#### 场景：落点统一
- **当** 新增或更新 baseline 相关代码
- **则** 实现文件和执行脚本必须落在 `/home/zcq/VDB/baselines`
- **且** 不得继续将错误旧脚本作为正式 baseline 入口

### 需求：结果必须分阶段输出关键指标
前置验证结束后必须输出统一的分阶段性能和精度指标。

#### 场景：结果字段完整
- **当** 前置验证完成
- **则** 输出必须包含 `recall@10`
- **且** 输出必须包含 `ann_core_ms`
- **且** 输出必须包含 `rerank_ms`
- **且** 输出必须包含 `payload_fetch_ms`
- **且** 输出必须包含 `e2e_ms`
