## Context

`crc-overlap-integration` 已完成 OverlapScheduler 的 CRC 集成（CrcStopper、动态 SafeOut、ClassifyAdaptive、est_heap_），但缺乏端到端验证。需要在 `bench_e2e` 中新增 CRC 对比模式，**从磁盘索引加载 ClusterData** 用于 CRC 标定，然后对比 baseline vs CRC 的搜索性能。

## Goals / Non-Goals

**Goals:**
- 在 bench_e2e 中新增 CRC 对比模式（`--crc` 开关）
- 从磁盘 `.clu` + `.dat` 文件加载 ClusterData（非内存 build 数据）
- 使用 `CrcCalibrator::CalibrateWithRaBitQ` 标定 CRC 参数
- 双轮查询：baseline（d_k 早停）vs CRC（CRC 早停 + 动态 SafeOut）
- 输出对比指标：recall、搜索时间、SafeOut 率/误杀率、早停率、overlap_ratio

**Non-Goals:**
- 修改 OverlapScheduler 核心逻辑（已在 crc-overlap-integration 中完成）
- 新建独立的 benchmark 文件（复用 bench_e2e）
- Prefetch 敏感性扫描（可后续单独实验）

---

## 架构概览

```
bench_e2e --crc 模式下的执行流:

Phase A: Load Data (npy)        ← 现有，不变
Phase B: Brute-Force GT         ← 现有，不变
Phase C: Build Index            ← 现有，不变
Phase C.5: CRC Calibration      ← 新增
  ├─ 从磁盘加载 ClusterData
  │   ├─ IvfIndex.Open(index_dir)
  │   ├─ For each cluster:
  │   │   ├─ Segment.GetBlockLocation(cid) → offset, size
  │   │   ├─ pread(.clu, offset, size) → block_buf
  │   │   ├─ Segment.ParseClusterBlock(cid, block_buf) → ParsedCluster
  │   │   │   ├─ codes_start (zero-copy RaBitQ codes)
  │   │   │   └─ decoded_addresses (offset+size in .dat)
  │   │   └─ For each record: Segment.ReadVector(addr) → raw float[dim]
  │   └─ Populate ClusterData[] (vectors, ids, codes_block, code_entry_size)
  ├─ Compute brute-force GT (uint32_t IDs for CRC calibrator)
  └─ CrcCalibrator::CalibrateWithRaBitQ() → CalibrationResults
Phase D: Query Round 1 (baseline)  ← 现有，crc_params=nullptr
Phase D.2: Query Round 2 (CRC)     ← 新增，crc_params 设为标定结果
Phase E: Evaluate & Compare         ← 扩展，双轮对比
Phase F: Output JSON                ← 扩展，新增 CRC 对比字段
```

---

## 详细设计

### 1. 磁盘 ClusterData 加载

CRC 标定需要 `ClusterData` 结构，其中 `vectors` 和 `codes_block` 必须从磁盘读取。

#### 加载流程

```cpp
// 已有的 IvfIndex 在 Phase D 已 Open，此处复用
auto& seg = index.segment();
uint32_t nlist = index.nlist();

// 每个 cluster 的数据
struct DiskClusterData {
    std::vector<float> vectors;              // [count × dim]
    std::vector<uint32_t> ids;               // record local index
    std::unique_ptr<uint8_t[]> block_buf;    // 拥有 .clu block 原始数据
    uint32_t count;
    const uint8_t* codes_start;              // 指向 block_buf 内
    uint32_t code_entry_size;
};

std::vector<DiskClusterData> disk_clusters(nlist);

for (uint32_t cid = 0; cid < nlist; ++cid) {
    auto loc = seg.GetBlockLocation(cid);     // O(1) 内存查找
    if (!loc) continue;

    // 同步 pread 读取 cluster block
    auto buf = std::make_unique<uint8_t[]>(loc->size);
    pread(seg.clu_fd(), buf.get(), loc->size, loc->offset);

    // 解析 block → ParsedCluster
    ParsedCluster pc;
    seg.ParseClusterBlock(cid, std::move(buf), loc->size, pc);

    // 从 .dat 读取原始向量
    uint32_t count = pc.num_records;
    disk_clusters[cid].vectors.resize(count * dim);
    disk_clusters[cid].ids.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        seg.ReadVector(pc.decoded_addresses[i],
                       disk_clusters[cid].vectors.data() + i * dim);
        disk_clusters[cid].ids[i] = i;  // 使用 cluster 内 local index
    }

    disk_clusters[cid].count = count;
    disk_clusters[cid].block_buf = std::move(pc.block_buf);
    disk_clusters[cid].codes_start = pc.codes_start;
    disk_clusters[cid].code_entry_size = pc.code_entry_size;
}
```

#### Vector ID 方案

CRC 标定中 `ClusterData::ids` 用于 ground truth 匹配。使用 **cluster 内 local index** (`0, 1, 2, ...`)，GT 也以相同方式计算。这避免了 image_id（int64_t）和 CrcCalibrator（uint32_t）的类型不匹配。

### 2. CRC 标定用 GT 计算

CrcCalibrator 的 GT 格式为 `vector<vector<uint32_t>>`（每个 query 的 top-k vector IDs）。由于使用 local index 作 ID，需要从加载的磁盘向量重新计算 brute-force GT：

```cpp
// 合并所有 cluster 向量到一个扁平数组
std::vector<float> all_vecs;
std::vector<uint32_t> all_ids;
uint32_t global_offset = 0;
for (uint32_t cid = 0; cid < nlist; ++cid) {
    for (uint32_t i = 0; i < disk_clusters[cid].count; ++i) {
        all_vecs.insert(all_vecs.end(),
            disk_clusters[cid].vectors.data() + i * dim,
            disk_clusters[cid].vectors.data() + (i + 1) * dim);
        all_ids.push_back(global_offset + i);
    }
    // 更新 ClusterData::ids 为 global offset
    for (uint32_t i = 0; i < disk_clusters[cid].count; ++i) {
        disk_clusters[cid].ids[i] = global_offset + i;
    }
    global_offset += disk_clusters[cid].count;
}

// Brute-force GT for CRC calibrator
std::vector<std::vector<uint32_t>> crc_gt(Q);
for (uint32_t qi = 0; qi < Q; ++qi) {
    // L2 distances to all_vecs → top-k → crc_gt[qi]
}
```

### 3. 双轮查询

```cpp
// Round 1: Baseline (d_k 早停 + 静态 SafeOut)
SearchConfig baseline_cfg;
baseline_cfg.top_k = GT_K;
baseline_cfg.nprobe = nprobe;
baseline_cfg.early_stop = true;
baseline_cfg.crc_params = nullptr;  // legacy mode

OverlapScheduler baseline_sched(index, reader, baseline_cfg);
for (auto& q : queries) baseline_results.push_back(baseline_sched.Search(q));

// Round 2: CRC (CRC 早停 + 动态 SafeOut)
SearchConfig crc_cfg = baseline_cfg;
crc_cfg.crc_params = &calib_results;
crc_cfg.initial_prefetch = 4;

OverlapScheduler crc_sched(index, reader, crc_cfg);
for (auto& q : queries) crc_results.push_back(crc_sched.Search(q));
```

### 4. 对比输出指标

```
┌─────────────────────┬──────────┬──────────┐
│ Metric              │ Baseline │   CRC    │
├─────────────────────┼──────────┼──────────┤
│ recall@1            │  0.xxxx  │  0.xxxx  │
│ recall@5            │  0.xxxx  │  0.xxxx  │
│ recall@10           │  0.xxxx  │  0.xxxx  │
│ avg_query_ms        │  x.xxx   │  x.xxx   │
│ p95_query_ms        │  x.xxx   │  x.xxx   │
│ avg_io_wait_ms      │  x.xxx   │  x.xxx   │
│ avg_probe_ms        │  x.xxx   │  x.xxx   │
│ overlap_ratio       │  0.xxx   │  0.xxx   │
│ early_stop_rate     │  xx.x%   │  xx.x%   │
│ avg_clusters_probed │  xx.x    │  xx.x    │
│ avg_safe_out        │  xx.x    │  xx.x    │
│ avg_safe_in         │  xx.x    │  xx.x    │
│ avg_uncertain       │  xx.x    │  xx.x    │
│ safe_out_false_prune│  0.xxxx  │  0.xxxx  │
└─────────────────────┴──────────┴──────────┘
```

`overlap_ratio = 1 - io_wait_time / total_time`

`safe_out_false_prune`: 需要额外逻辑统计被 SafeOut 的向量中有多少在精确 top-k 中（误杀率）。此指标通过对比 CRC 模式的 predicted_ids 与 brute-force GT 的差异间接反映。

### 5. 命令行参数

```
--crc              启用 CRC 对比模式（默认关闭）
--crc-alpha 0.1    CRC 标定 FNR 上界
--crc-calib 0.5    标定查询比例
--crc-tune  0.1    调参查询比例
```

### 6. CMakeLists.txt 变更

```cmake
# bench_e2e 新增 vdb_crc 链接
target_link_libraries(bench_e2e PRIVATE vdb_query vdb_io vdb_crc)
```

---

## Decisions

### D1: 复用 bench_e2e 而非新建文件
- bench_e2e 已有完整的 build→query→evaluate pipeline
- 通过 `--crc` 开关启用，不影响现有行为
- 避免重复 npy 加载、GT 计算、JSON 输出逻辑

### D2: ClusterData 从磁盘加载
- 实验设定要求从磁盘读取，模拟真实部署场景
- 通过 `Segment.GetBlockLocation` + `pread` + `ParseClusterBlock` 加载 codes_block
- 通过 `Segment.ReadVector` 加载原始向量

### D3: Vector ID 使用 global offset (非 image_id)
- CrcCalibrator 使用 uint32_t IDs
- bench_e2e 的 GT 使用 int64_t image_ids
- 解法：CRC 标定单独计算基于 global offset 的 GT，不复用 Phase B 的 GT
- 搜索结果的 recall 仍然用 Phase B 的 image_id GT

### D4: 双轮查询共享同一 IvfIndex 实例
- IvfIndex 只读，两轮查询可安全共享
- IoUringReader 也可共享（每轮 Search 内部管理 SQE/CQE 生命周期）

---

## Risks / Trade-offs

- **[磁盘加载开销]** 同步 pread 所有 cluster blocks + ReadVector 较慢，但只在标定阶段执行一次，可接受
- **[内存峰值]** 同时持有所有 cluster 的原始向量（N × dim × 4 bytes），coco_1k 约 2MB，可接受；大数据集可考虑采样
- **[两轮查询的缓存效应]** Round 2（CRC）可能受益于 Round 1 的 page cache 预热。可通过 `posix_fadvise(POSIX_FADV_DONTNEED)` 或先跑 CRC 后跑 baseline 来缓解，但暂不做
