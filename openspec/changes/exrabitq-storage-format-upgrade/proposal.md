## Why

现有 `IPExRaBitQ` 的第一阶段 kernel 重写已经把“重建 signed float(code)”这一层额外开销压掉，但 clean perf 仍显示 Stage2 是 resident + IVF 查询主路径中的显著 CPU 热点。根因已经从纯计算逻辑收敛为存储格式与访存组织本身：`ex_code[dim bytes] + ex_sign[dim bytes] + xipnorm` 的 Region2 布局让 sign 以 `1 byte / dim` 冗余存储，并继续把查询期耗时浪费在 sign 带宽、mask 生成和 AoS entry 级 decode 上，因此需要单独开一个 change，把 ExRaBitQ 的存储格式升级、版本迁移和 benchmark 验收边界细化清楚。

## What Changes

- 为 ExRaBitQ Stage2 新增一套独立的存储格式升级 change，明确将 kernel 第一阶段优化与 `.clu` 持久化格式升级解耦。
- 在 `.clu` 的 Region2 中引入新的 ExRaBitQ entry 版本：
  - 将 `ex_sign` 从逐维 `uint8` 改为 bit-packed sign storage
  - 保留 `ex_code` 与 `xipnorm` 的数学语义不变
  - **BREAKING**：升级 `.clu` 文件版本，并要求 reader 按文件版本显式解析旧/新布局
- 为第二阶段继续预留 `compact ex-code layout` 的扩展位，但本 change 的第一目标是先落地 `sign bit-pack` 和对应的 AVX-512 packed-sign query kernel。
- 明确编码、cluster store、resident preload、parsed cluster view、query kernel 的新边界：
  - 编码输出不再把 `ex_sign` 的逐维 byte 形式作为持久化主表示
  - 读取器和 resident view 必须向查询路径暴露 packed-sign 访问接口
  - `IPExRaBitQ` 必须支持直接消费 packed-sign，而不是继续假设 `sign[dim bytes]`
- 明确格式升级后的迁移和回滚策略：
  - 旧格式 reader 兼容策略
  - 新格式 rebuild 要求
  - resident / benchmark 元数据中暴露实际使用的 ExRaBitQ storage version
- 补充专项 benchmark / perf 验收：
  - full E2E 和 query-only clean perf 继续双口径
  - 必须验证 recall、排序语义、Stage2 时间和热点迁移
  - 必须能区分“packed-sign 真收益”与“版本/打点带来的假变化”

## Capabilities

### New Capabilities
- `exrabitq-storage-layout`: 定义 ExRaBitQ Region2 的版本化存储布局、packed-sign entry 结构、迁移与回滚要求。

### Modified Capabilities
- `query-pipeline`: 查询路径需要支持从 packed-sign ExRaBitQ entry 直接完成 Stage2 估计，并保持现有 recall、排序和 resident serving 语义。
- `e2e-benchmark`: benchmark 输出需要显式记录 ExRaBitQ storage 版本、query-only 与 full E2E 双口径下的 Stage2 验收结果，以及 rebuild / format 信息。

## Impact

- 主要影响 [include/vdb/rabitq/rabitq_encoder.h](/home/zcq/VDB/VectorRetrival/include/vdb/rabitq/rabitq_encoder.h)、[src/rabitq/rabitq_encoder.cpp](/home/zcq/VDB/VectorRetrival/src/rabitq/rabitq_encoder.cpp)、[include/vdb/storage/cluster_store.h](/home/zcq/VDB/VectorRetrival/include/vdb/storage/cluster_store.h)、[src/storage/cluster_store.cpp](/home/zcq/VDB/VectorRetrival/src/storage/cluster_store.cpp)、[include/vdb/query/parsed_cluster.h](/home/zcq/VDB/VectorRetrival/include/vdb/query/parsed_cluster.h)、[src/simd/ip_exrabitq.cpp](/home/zcq/VDB/VectorRetrival/src/simd/ip_exrabitq.cpp) 以及 benchmark 结果回填路径。
- 该 change 会引入 `.clu` 文件版本升级，并影响 cluster build / load / resident preload / Stage2 query kernel 的协同关系。
- 不引入新的外部依赖；但会要求对既有索引文件提供明确的兼容、重建或拒绝策略，避免旧 reader 静默错误解析新格式。
