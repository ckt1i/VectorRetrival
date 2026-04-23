## 1. Compact Stage2 Layout Definition

- [x] 1.1 为 Stage2 `v11` compact layout 定义 header、batch block、`valid_count`、`batch_size=8`、`dim_block=64` 的序列化结构
- [x] 1.2 在 cluster build / serialize 路径中实现 `v11` Stage2 region 写出逻辑，确保直接生成 compact blocked 布局
- [x] 1.3 保留 `v10 packed_sign` 读取兼容，并让索引元数据明确区分 `v10` 与 `v11`

## 2. Open / Parse Path

- [x] 2.1 在 cluster store / parser 中按 Stage2 版本分流 `v10` 与 `v11`
- [x] 2.2 为 `v11` 新增 Stage2 batch-block 视图，暴露 compact abs/sign/xipnorm block 与 `valid_count`
- [x] 2.3 确认尾块 padding 不会被解析成真实 candidate

## 3. Stage2 Block-Aware Scheduling

- [x] 3.1 在 query path 中把 Stage1 uncertain 从 candidate list 映射为 `batch_block_id + lane_id`
- [x] 3.2 为每个 Stage2 batch block 生成 `lane_mask` 并按 block 顺序驱动 true batch kernel
- [x] 3.3 保持 Stage1 funnel、top-k、CRC、payload 和 resident serving 语义不变

## 4. Batch Kernel Integration

- [x] 4.1 让 true batch kernel 直接消费 `v11` compact block 视图，而不是 8 个散指针
- [x] 4.2 处理尾块 `valid_count` 与 `lane_mask`，确保 padded lane 不参与 Stage2 boosting
- [x] 4.3 保留 `v10` 兼容路径用于对比和回退，不改变现有用户可见搜索接口

## 5. Rebuild And Validation

- [x] 5.1 用相同参数重建 `v11` compact index
- [x] 5.2 跑 `query-only + fine_grained_timing=0` clean perf，对比 `v10` 与 `v11` 的 `avg_query_ms`、`probe_stage2_ms` 与热点迁移
- [x] 5.3 跑同口径 full E2E，对比 `v10` 与 `v11` 的 recall、avg/p50/p95/p99 与 query-path breakdown
- [x] 5.4 汇总 compact layout / rebuild 是否真正改善 Stage2 带宽瓶颈，并形成后续是否继续优化 kernel/block layout 的结论
