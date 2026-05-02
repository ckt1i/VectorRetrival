## 为什么

当前 `coco100k-top10-precheck` 的 baseline 定义需要纠偏。

前一版方案把 `IVF + PQ + FlatStor`、`IVF + RaBitQ + FlatStor` 和 `bench_e2e` 都包装在同一条 `bench_e2e` 路径上，导致：
- `IVF + PQ` 没有真正走 Faiss `IndexIVFPQ`
- `IVF + RaBitQ` 没有真正走 `RaBitQ-Library`
- 所谓 `FlatStor` 也没有形成独立的原始向量与原始数据读取后端
- 三组结果实际上不具备有效的 baseline 对比意义

因此，这个 change 需要更新为一版真实的前置验证方案：
- `IVF + PQ baseline` 改为真正的 Faiss `IndexIVFPQ` 路径
- `IVF + RaBitQ baseline` 改为真正的 `RaBitQ-Library` IVF 路径
- 原始向量 rerank 与原始数据读取从向量检索 core 中拆开，统一进入共享后端
- 前置验证继续只覆盖 `coco_100k`、`top10`，但验证对象改为“两个真实 baseline 是否已经具备公平比较条件”

## 变更内容

- 将 `coco100k-top10-precheck` 的 baseline 定义更新为两条真实向量检索路径：
  - `Faiss IVFPQ`
  - `RaBitQ-Library IVF + RaBitQ`
- 将旧的 `bench_e2e` 方案从 baseline 身份降级为可选参考，不再作为 `IVF + PQ` 或 `IVF + RaBitQ` 的实现承载。
- 将原始向量 rerank 与原始数据读取改为共享后端，不再复用当前仓库里的 `data.dat / cluster.clu` 统一段文件来冒充 `FlatStor`。
- 统一要求 baseline 实现落在 `/home/zcq/VDB/baselines` 路径下，包括：
  - baseline driver
  - 构建脚本
  - 数据准备脚本
  - precheck 执行脚本
- `coco_100k` 的聚类继续固定复用以下文件：
  - `/home/zcq/VDB/data/coco_100k/coco100k_centroid_2048.fvecs`
  - `/home/zcq/VDB/data/coco_100k/coco100k_cluster_id_2048.ivecs`
- 前置验证继续冻结：
  - `dataset=coco_100k`
  - `topk=10`
  - `nlist=2048`
  - `nprobe=64`
  - `queries=1000`
  - 候选扩展系数 `n=20`，即先取 `top-(20 * topk)=200` 候选，再做 rerank

## 能力

### 修改能力
- `coco100k-top10-precheck`: 将前置验证重新定义为“真实 Faiss IVFPQ / RaBitQ-Library IVF + 共享 rerank/payload 后端”的最小闭环验证

### 新增能力
- `faiss-ivfpq-precheck-driver`: 基于真实 Faiss `IndexIVFPQ` 的 `coco_100k top10` baseline driver
- `rabitq-lib-precheck-driver`: 基于真实 `RaBitQ-Library` IVF 的 `coco_100k top10` baseline driver
- `shared-rerank-payload-backend`: 为两个 baseline 提供统一的原始向量 rerank 与原始数据读取后端

## 影响范围

- 受影响的文档：
  - `openspec/changes/coco100k-top10-precheck/*`
- 受影响的目录：
  - `/home/zcq/VDB/baselines`
- 受影响的第三方依赖：
  - `/home/zcq/VDB/third_party/faiss`
  - `/home/zcq/VDB/third_party/RaBitQ-Library`
- 受影响的输出：
  - 候选集导出结果
  - `recall@10`
  - `ann_core_ms`
  - `rerank_ms`
  - `payload_fetch_ms`
  - `e2e_ms`
