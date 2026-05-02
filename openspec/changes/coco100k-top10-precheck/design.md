## 背景

这个 change 的目标不再是“把 `bench_e2e` 包装成两个 baseline 名字”，而是先在 `coco_100k` 上建立一个真实可信的 baseline 对比框架。

经过对 `/home/zcq/VDB/third_party` 的检查，已经明确：
- Faiss 具备真正的 `IndexIVFPQ`、`IndexRefineFlat`、`search_preassigned` 等能力
- `RaBitQ-Library` 具备真正的 `IVF + RaBitQ` indexing/querying 路径
- 但 `RaBitQ-Library` 的 IVF 原生查询默认不访问原始向量
- 当前仓库中的 `data.dat / cluster.clu` 并不是独立的 `FlatStor` backend

因此，本次 precheck 的设计要从“错误的单路径伪对比”改成“三段式”：

```text
向量检索 core
  ├─ Faiss IVFPQ
  └─ RaBitQ-Library IVF + RaBitQ
          ↓
共享 rerank
  └─ 统一读取原始向量，对 top-200 候选做精确距离重排
          ↓
共享 payload 读取
  └─ 统一按列读取 final top-10 对应原始数据
```

## 目标 / 非目标

**目标：**
- 在 `coco_100k` 上建立真实可复验的 `IVF + PQ` 与 `IVF + RaBitQ` baseline
- 让两个 baseline 共用同一份聚类、同一份原始向量、同一份 payload 后端
- 将向量检索时间、rerank 时间、payload 读取时间分开记录
- 让后续正式实验可以在这个基础上扩到更多数据集

**非目标：**
- 不再将 `bench_e2e` 作为两个 baseline 的真实实现
- 不在这一步扩到 `coco_1m`、`deep1m_synth`、`deep8m`
- 不在这一步做 `top50/top100`
- 不在这一步做 `nprobe sweep`
- 不在这一步做不同 payload backend 比较

## 决策

### 1. `IVF + PQ` 必须使用真实 Faiss `IndexIVFPQ`

选择：
- `IVF + PQ baseline` 使用 Faiss 原生 `IndexIVFPQ`

原因：
- 这是唯一符合 baseline 名称语义的路径
- 可以直接控制 `nlist`、`nprobe`、`M`、`nbits`
- 可以显式导出候选集，而不是借用仓库内的其他检索实现

实现约束：
- 优先复用现成 2048 聚类
- 必须支持先取 `top-200` 候选
- rerank 不依赖 `IndexRefineFlat` 直接作为最终方案，因为 `IndexRefineFlat` 更偏内存内 refine，不等于统一磁盘原始向量读取口径

### 2. `IVF + RaBitQ` 必须使用真实 `RaBitQ-Library` IVF

选择：
- `IVF + RaBitQ baseline` 使用 `RaBitQ-Library` 的 IVF index/query 路径

原因：
- 它才是正确的 baseline 实现来源
- 其聚类前处理本身也与 Faiss 兼容

实现约束：
- 必须复用现成 2048 聚类文件
- 不能继续以 `bench_e2e bits=1` 冒充 `IVF + RaBitQ`
- 需要补一个轻量 adapter，使查询阶段能输出不少于 `top-200` 的候选结果，供统一 rerank 使用

### 3. rerank 与 payload 读取必须从向量检索 core 解耦

选择：
- 两条 baseline 都只负责产生候选集
- rerank 与 payload 读取统一走共享后端

原因：
- Faiss 与 `RaBitQ-Library` 原生查询对“是否访问原始向量”的语义不同
- 如果把 rerank 逻辑埋到各自库内部，后续就无法做公平的阶段拆分统计
- 统一后端更利于后续扩展到 `deep1m_synth` 等数据集

统一契约：
- 输入：candidate ids，数量固定为 `200`
- rerank：从共享 raw-vector store 读取原始向量，计算精确距离，得到 final top-10
- payload fetch：从共享列存 payload store 读取 final top-10 的原始数据

### 4. `FlatStor` 在这个 change 中必须被重新定义为共享后端契约

选择：
- 本次 change 中的 `FlatStor` 不再指代旧有 `data.dat / cluster.clu`
- 改为明确的共享存储契约

建议契约：
- `raw_vectors`：按 id 对齐的原始向量存储
- `payload columns`：按列存储的原始数据
- `id -> offset` 映射
- 支持批量按 id 拉取向量与 payload

原因：
- 当前代码库里没有真正独立的 `FlatStor` 实现
- 不先把契约写清，后续脚本和 benchmark 仍会语义混乱

### 5. `bench_e2e` 只保留为可选参考，不进入主 gate

选择：
- `bench_e2e` 可以保留作兼容性参考
- 但不作为 `IVF + PQ` 或 `IVF + RaBitQ` 的主执行入口

原因：
- 它已经被证明不能代表这两个 baseline
- 继续把它放在主 gate 会混淆结果解读

## 执行模型

```text
数据准备
  ├─ coco_100k base/query/gt
  ├─ coco100k_centroid_2048.fvecs
  └─ coco100k_cluster_id_2048.ivecs

向量检索 core
  ├─ Faiss IVFPQ driver
  │   └─ 输出 top-200 candidate ids
  └─ RaBitQ-Library IVF driver
      └─ 输出 top-200 candidate ids

共享后处理
  ├─ raw-vector rerank
  │   └─ 输出 final top-10
  └─ payload fetch
      └─ 读取 final top-10 原始数据

结果汇总
  ├─ recall@10
  ├─ ann_core_ms
  ├─ rerank_ms
  ├─ payload_fetch_ms
  └─ e2e_ms
```

## 编译与运行参数

### Faiss 侧
- 使用真实 Faiss 构建产物
- 默认要求开启 CPU 优化版本
- 具体 `M`、`nbits` 作为 baseline 配置显式记录

### RaBitQ-Library 侧
- 使用 `RaBitQ-Library` 自带 sample/binary 或等价轻量封装
- 继续使用 `coco_100k` 的现成 `.fvecs / .ivecs`

### 前置验证冻结参数
- `dataset=coco_100k`
- `topk=10`
- `nlist=2048`
- `nprobe=64`
- `queries=1000`
- 候选扩展系数 `20`

## 风险 / 权衡

- [风险] `RaBitQ-Library` IVF sample 默认只输出 top-k，不能直接满足 top-200 rerank。
  → 处理方式：补一个最薄的 querying adapter，只扩展候选导出，不重写其核心检索逻辑。

- [风险] Faiss `IndexRefineFlat` 与共享磁盘 rerank 语义不同。
  → 处理方式：将 `IndexRefineFlat` 只作为能力参考，不作为正式 precheck 主路径。

- [风险] 共享 payload 后端目前还只是契约，不是现成实现。
  → 处理方式：先把契约写入 change，并把实现拆成独立任务，避免继续混用错误旧路径。
