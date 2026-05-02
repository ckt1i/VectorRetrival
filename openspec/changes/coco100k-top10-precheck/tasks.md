## 1. 文档纠偏

- [x] 1.1 移除当前 change 中把 `bench_e2e` 伪装为 `IVF + PQ` 与 `IVF + RaBitQ` 的描述
- [x] 1.2 明确 `data.dat / cluster.clu` 不再被视为本 change 中的 `FlatStor`
- [x] 1.3 将 `/home/zcq/VDB/baselines` 固定为 baseline 实现与执行脚本的统一目录

## 2. 共享数据契约准备

- [x] 2.1 固定 `coco_100k` 为唯一 precheck 数据集
- [x] 2.2 固定 `topk=10`
- [x] 2.3 固定 `queries=1000`
- [x] 2.4 固定 `nlist=2048`
- [x] 2.5 固定 `nprobe=64`
- [x] 2.6 固定候选扩展系数 `20`，即每次先取 `top-200`
- [x] 2.7 复用 `/home/zcq/VDB/data/coco_100k/coco100k_centroid_2048.fvecs`
- [x] 2.8 复用 `/home/zcq/VDB/data/coco_100k/coco100k_cluster_id_2048.ivecs`
- [x] 2.9 定义共享 `raw-vector store` 的文件格式、id 对齐规则和批量读取接口
- [x] 2.10 定义共享 `payload store` 的列存格式、offset 契约和批量读取接口

## 3. Faiss IVFPQ baseline

- [x] 3.1 在 `/home/zcq/VDB/baselines` 下新增真实 Faiss `IndexIVFPQ` driver
- [x] 3.2 为 Faiss baseline 增加独立执行脚本
- [x] 3.3 让 Faiss baseline 复用现成 2048 centroids，并以真实 `IndexIVFPQ` 路径完成建索引与查询
- [x] 3.4 让 Faiss baseline 输出 `top-200 candidate ids`
- [x] 3.5 记录 Faiss baseline 的 `ann_core_ms`

## 4. RaBitQ-Library baseline

- [x] 4.1 在 `/home/zcq/VDB/baselines` 下新增真实 `RaBitQ-Library` IVF driver
- [x] 4.2 为 `RaBitQ-Library` baseline 增加独立构建脚本
- [x] 4.3 让 `RaBitQ-Library` baseline 复用现成 2048 聚类
- [x] 4.4 为 `RaBitQ-Library` 查询增加候选导出 adapter，使其输出 `top-200 candidate ids`
- [x] 4.5 记录 `RaBitQ-Library` baseline 的 `ann_core_ms`

## 5. 共享 rerank 与 payload 读取

- [x] 5.1 新增共享 rerank 模块，输入 candidate ids，读取原始向量并做精确重排
- [x] 5.2 新增共享 payload 读取模块，只读取 final top-10 的原始数据
- [x] 5.3 分别记录 `rerank_ms` 与 `payload_fetch_ms`
- [x] 5.4 计算统一 `e2e_ms`

## 6. precheck 执行脚本

- [x] 6.1 在 `/home/zcq/VDB/baselines` 下新增 `Faiss IVFPQ + shared rerank/payload` precheck 脚本
- [x] 6.2 在 `/home/zcq/VDB/baselines` 下新增 `RaBitQ-Library IVF + shared rerank/payload` precheck 脚本
- [x] 6.3 如有需要，保留 `bench_e2e` 参考脚本，但标注为非 gate、非 baseline
- [x] 6.4 统一日志目录、输出目录和结果摘要格式

## 7. 结果检查

- [x] 7.1 检查两条 baseline 是否都真正来自各自第三方实现
- [x] 7.2 检查两条 baseline 是否都使用同一份 2048 centroids，且 `RaBitQ-Library` 复用了同一份 cluster ids
- [x] 7.3 检查两条 baseline 是否都输出 `top-200` 候选
- [x] 7.4 检查统一 rerank 后的 `recall@10`
- [x] 7.5 检查 `ann_core_ms / rerank_ms / payload_fetch_ms / e2e_ms`
- [x] 7.6 输出前置验证结论，确认是否可以进入后续正式 baseline 扩展
