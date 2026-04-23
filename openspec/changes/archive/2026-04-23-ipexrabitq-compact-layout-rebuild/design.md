## Context

当前 `v10 packed_sign` Stage2 region 仍是按单 candidate 顺序存储：
- `code_abs[dim]`
- `packed_sign[ceil(dim/8)]`
- `xipnorm`

这种 AoS / per-vector 布局适合单 candidate kernel 顺序扫描，但不适合 `batch=8` 的 true multi-candidate Stage2 kernel。此前已经验证两种失败路径：
- `batch 调度壳 + 单 candidate kernel`
- `true batch kernel + 旧 v10 layout`

两者都退化，说明瓶颈不只是 kernel，而是 kernel 与 layout 不匹配。要让 Stage2 真正按 batch 受益，必须把磁盘布局、解析视图和查询调度同时改成 block-aware 形态。

## Goals / Non-Goals

**Goals:**
- 定义 `v11` Stage2 compact blocked layout，固定为 `batch_size=8`、`dim_block=64`。
- 明确 rebuild 路径：build/serialize 直接生成 `v11`，query 不做在线 repack。
- 为 `ParsedCluster` 提供 batch-block 视图，替代仅按单 candidate 暴露 Stage2 数据。
- 将 Stage2 调度改成 `block_id + lane_mask` 驱动的 true batch boosting。
- 保持 Stage1 funnel、top-k、CRC、payload 和 resident serving 语义不变。

**Non-Goals:**
- 不修改 Stage1 FastScan 布局，不修改 coarse 路径，不修改 submit / rerank 语义。
- 不要求删除 `v10 packed_sign` 读取兼容；旧索引仍可读，但 compact 路线正式依赖 rebuild。
- 不在本轮引入新的 benchmark CLI，也不把 compact layout 与其它优化 change 混做。

## Decisions

### 决策一：`v11` 采用 batch-major blocked Stage2 layout

`v11` Stage2 region 采用如下逻辑布局：

```text
cluster_stage2_region_v11 =
  [header]
  [batch_block_0]
  [batch_block_1]
  ...
```

每个 `batch_block` 固定对应 8 个 candidate（尾块不足 8 时补齐，但记录真实 lane 数），内部按 `dim_block=64` 组织：

```text
batch_block:
  for dim_block in 0..ceil(dim/64)-1:
    abs_plane[8][64]      // uint8
    sign_plane[8][8B]     // 64 sign bits
  xipnorm[8]              // float
  valid_count             // 1..8
```

物理存储采用“一个 dim block 的 8 个 lane 连续存放，再进入下一个 dim block”的顺序，而不是继续沿用 per-vector layout。这样 batch kernel 在一个 dim block 内可以一次读 query、连续消费 8 个 candidate 的 abs/sign block。

备选方案：继续在 query 时从 `v10` repack 成 batch-friendly layout。  
放弃原因：在线 repack 会把磁盘 layout 缺失的成本搬到 query 主路径，抵消 Stage2 kernel 收益。

### 决策二：Stage2 rebuild 是必需步骤，不做在线格式升级

compact layout 不通过“打开旧索引时自动转换”引入，而是作为新的 Stage2 持久化版本写入新索引。  
也就是说：
- build 阶段直接序列化 `v11`
- open/query 阶段只负责识别版本并按对应 parser 读取

备选方案：保留 `v10` 文件格式，查询时临时拼成 block8。  
放弃原因：已验证 query-side 组织开销会放大热点。

### 决策三：`ParsedCluster` 新增 batch-block 视图，而不是只暴露单 candidate view

为 `v11` 新增 Stage2 batch 视图，建议形态固定为：

```text
ExRaBitQBatchBlockView {
  const uint8_t* abs_blocks;
  const uint8_t* sign_blocks;
  const float* xipnorms;
  uint32_t valid_count;
  uint32_t batch_block_id;
}
```

它表达的是“一个 batch block 的 Stage2 读视图”，而不是单 candidate 的指针集合。  
单 candidate `ExRaBitQView` 仍可保留给 `v10` 兼容路径，但 `v11` 正式路径必须优先走 batch-block view。

### 决策四：Stage2 调度改成 `block_id + lane_mask`

对于 `v11`，Stage2 不再把 uncertain 表示成“candidate list + 8 个散指针”，而是：

```text
global_idx
  -> batch_block_id = global_idx / 8
  -> lane_id = global_idx % 8
```

调度器先把 Stage1 uncertain 按 `batch_block_id` 分桶，再为每个 block 生成 `lane_mask`。  
Stage2 kernel 的调用形式固定为：
- 输入：`ExRaBitQBatchBlockView + lane_mask + query`
- 行为：只对 mask 覆盖的 lane 做 boosting 和分类

这保证 query path 与 compact layout 对齐，不再回到“收集 8 个散 candidate ptr”的模式。

### 决策五：第一版 true batch kernel 与 layout 参数锁死

第一版固定参数：
- `batch_size = 8`
- `dim_block = 64`
- `abs = uint8`
- `sign = packed bits`
- `xipnorm = float[8]`

不做：
- `dim=512` 特化格式
- 动态 batch size
- 额外页面分段格式

这样可以先把 layout、parser、scheduler、kernel 的边界锁死，避免实现时继续分叉。

## Risks / Trade-offs

- [Rebuild 成本上升] → 通过 benchmark 同时保留 `v10` 与 `v11` 对比输出，确认收益足以覆盖 rebuild 复杂度。
- [Stage2 parser 与 query path 变复杂] → 限制本轮只动 Stage2 region，不碰 Stage1 / coarse / payload 布局。
- [尾块 padding 浪费空间] → 固定用 `valid_count` 控制真实 lane 数，牺牲少量尾块空间换取 query 主路径简单。
- [兼容路径长期共存] → `v10` 只保留读取兼容和对比用途，正式优化路径以 `v11` 为准。
- [lane_mask 调度引入额外 bookkeeping] → 采用固定 `block_id = idx / 8`、`lane_id = idx % 8` 规则，避免复杂映射。

## Migration Plan

1. 在 cluster build / serialize 路径引入 `v11 compact layout` 写出逻辑。
2. 在 open / parse 路径中按存储版本区分 `v10` 与 `v11`，为 `v11` 产出 batch-block view。
3. 在 Stage2 query path 中加入 `block_id + lane_mask` 调度，并接入 batch kernel。
4. 使用同参数重建 `v11` 索引，跑 query-only perf 和 full E2E。
5. 若 `v11` 仍未带来收益，再复盘是否需要更激进的 block 内布局或更高阶 kernel；若收益成立，则逐步把正式实验口径切到 `v11`。

## Open Questions

- `v11` header 中是否显式记录 `batch_size` 和 `dim_block`，还是固定由版本号隐含。
- Stage2 kernel 第一版是否直接消费 `lane_mask`，还是调度侧先压缩成连续有效 lane。
- `xipnorm[8]` 是否应继续放在 batch block 末尾，还是前移到每个 dim block 前作为 cache locality 优化。
