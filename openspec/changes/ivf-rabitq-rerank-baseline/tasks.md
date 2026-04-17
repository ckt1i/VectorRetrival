## 1. Change Scaffolding And Dependency Setup

- [x] 1.1 在 `/home/zcq/VDB/third_party` 下创建独立的 `ivf-rabitq-rerank-baseline` 工程目录与基础构建脚手架
- [x] 1.2 固定并记录官方 `RaBitQ-Library` 与 Faiss 的依赖接入方式、版本约束和本地查找逻辑
- [x] 1.3 定义 baseline 工程的目录结构，至少区分 `builder`、`search`、`storage`、`cli` 和 `tests`

## 2. Storage Layout

- [x] 2.1 实现 `meta.json` schema，覆盖 `dim`、`metric`、`nlist`、`ntotal`、`code_size`、版本号和 build 参数摘要
- [x] 2.2 实现 `coarse_centroids.bin` 的导出与加载
- [x] 2.3 实现 `cluster_offsets.bin` 的导出与加载
- [x] 2.4 定义并实现 `clusters.clu` 的 block 格式，至少包含 `cluster_id`、`num_records`、`row_ids`、`rq_codes` 和最小 side stats
- [x] 2.5 实现 `vectors.bin` 的扁平 raw vector 导出与按 `row_id` 定长寻址读取
- [x] 2.6 为所有磁盘文件补齐版本校验、尺寸校验和基本损坏检查

## 3. Build Path

- [x] 3.1 实现 base vector 读取入口，支持 formal-study 当前使用的主向量输入格式
- [x] 3.2 使用 Faiss 完成 coarse quantizer 训练，并暴露 `--nlist`、`--metric`、`--train-size`
- [x] 3.3 实现 base vectors 到 coarse clusters 的 assignment，并生成每个 cluster 的 posting list
- [x] 3.4 调用官方 `RaBitQ-Library` 生成 cluster 内记录的 compressed representation
- [x] 3.5 将 assignment 结果和 RaBitQ codes 写入 `clusters.clu` 与 `cluster_offsets.bin`
- [x] 3.6 在 build 完成后导出 `vectors.bin` 和构建摘要，便于后续 rerank 与 formal-study 记录

## 4. Search Path

- [x] 4.1 实现 query 输入与参数解析，至少支持 `--index-dir`、`--queries`、`--raw-vectors`、`--nprobe`、`--candidate-budget`、`--topk`
- [x] 4.2 使用官方 `RaBitQ-Library` 的 IVF index load/search 执行 compressed candidate search，并暴露 `nprobe`
- [x] 4.3 导出并校验 `cluster_offsets.bin` / `clusters.clu` sidecar，供审计与后续扩展使用
- [x] 4.4 使用官方 `RaBitQ-Library` 完成 candidate generation，并以 `candidate_budget` 作为压缩检索返回候选数
- [x] 4.5 实现基于 `vectors.bin` 的 exact rerank，输出 final top-k ids
- [x] 4.6 为 search path 导出统一 metrics，至少包含 `vec_search_ms`、`rerank_ms`、`topk`、`nprobe`、`candidate_budget`

## 5. CLI And Experiment Integration

- [x] 5.1 提供 build CLI，显式暴露输入路径、输出路径、`nlist`、metric、训练规模和 RaBitQ 参数
- [x] 5.2 提供 search CLI，显式暴露 `nprobe`、`candidate_budget`、`topk`、query 路径和输出路径
- [x] 5.3 定义 search 输出文件格式，使其可被 formal-study 脚本直接消费或轻量包装
- [x] 5.4 补一个最小 formal-study wrapper，验证该 baseline 可接入当前实验计划

## 6. Verification

- [x] 6.1 为 storage reader/writer 增加单元测试，验证 `clusters.clu`、`cluster_offsets.bin` 和 `vectors.bin` 的一致性
- [x] 6.2 为 build/search 增加 smoke test，至少覆盖 `coco_100k` 小规模子集
- [x] 6.3 验证不同 `nlist`、`nprobe`、`topk` 参数能够通过 CLI 正确生效并写入输出元数据
- [x] 6.4 产出一份集成说明，说明该 baseline 与现有 `IVF+PQ+rerank` / formal-study 的对应关系
