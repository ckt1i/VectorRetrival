## ADDED Requirements

### Requirement: RotationMatrix Save 写入 Hadamard 状态
`RotationMatrix::Save()` SHALL 在 dense matrix 数据之后写入 flags(u8) 字段。当 `use_fast_hadamard_=true` 时，flags bit 0 = 1，并追加 diag_signs(dim × int8)。当 `use_fast_hadamard_=false` 时，flags = 0，无 diag_signs 段。此格式不向后兼容旧版 Load。

#### Scenario: Hadamard rotation 保存新格式
- **WHEN** 调用 `rotation.GenerateHadamard(seed, true)` 后 `rotation.Save(path)`
- **THEN** 文件大小 = 4 + dim×dim×4 + 1 + dim bytes，flags=1，diag_signs 段长度 = dim

#### Scenario: General rotation 保存新格式
- **WHEN** 调用 `rotation.GenerateRandom(seed)` 后 `rotation.Save(path)`
- **THEN** 文件大小 = 4 + dim×dim×4 + 1 bytes，flags=0，无 diag_signs 段

### Requirement: RotationMatrix Load 恢复 Hadamard 状态
`RotationMatrix::Load()` SHALL 在读取 dense matrix 后读取 1 字节 flags。若 `flags & 1`，SHALL 继续读取 dim 个 int8 diag_signs，设置 `use_fast_hadamard_=true` 和 `diag_signs_`。若 `flags == 0`，SHALL 设置 `use_fast_hadamard_=false`。

#### Scenario: 加载 Hadamard rotation 新格式
- **WHEN** `RotationMatrix::Load(path, dim)` 且文件含 flags=1 + diag_signs
- **THEN** 返回的 RotationMatrix 的 `is_fast_hadamard()` 返回 true，`diagonal_signs()` 长度 = dim

#### Scenario: 加载 General rotation 新格式
- **WHEN** `RotationMatrix::Load(path, dim)` 且文件含 flags=0
- **THEN** 返回的 RotationMatrix 的 `is_fast_hadamard()` 返回 false

### Requirement: 重建 COCO 100K 索引
使用现有 `bench_e2e` 的 build 路径（不指定 `--index-dir`）重建 COCO 100K 索引。重建后的索引 SHALL 包含：
1. 新格式 `rotation.bin`（含 flags + diag_signs）
2. `rotated_centroids.bin`（builder 已支持）
3. segment.meta 中 `has_rotated_centroids=true`

#### Scenario: 重建索引包含完整 Hadamard 支持
- **WHEN** 运行 `bench_e2e --dataset coco_100k` 不指定 `--index-dir`（触发 build）
- **THEN** 生成的 index 目录包含 `rotation.bin`（新格式）、`rotated_centroids.bin`、且 segment.meta 的 `has_rotated_centroids=true`

#### Scenario: 重建后 IvfIndex::Open 自动启用快速路径
- **WHEN** 对重建的索引调用 `IvfIndex::Open(dir)`
- **THEN** `used_hadamard()` 返回 true，`rotation().is_fast_hadamard()` 返回 true

### Requirement: 方案 A 效果验证
修复 Rotation 并重建索引后，SHALL 运行 `--nprobe-sweep 50,100,150,200 --queries 500` 并对比修复前数据（`/tmp/bench_sweep_full/.../nprobe_sweep.csv`），确认：
1. recall@10 不退化（容差 ±0.005）
2. probe_time_ms 显著下降
3. 新的 timing 分布（特别是 uring_submit_ms 占比变化）

#### Scenario: 修复后 recall 不退化
- **WHEN** 在重建的 COCO 100K 索引上运行 nprobe=200, queries=500
- **THEN** recall@10 与修复前差异 < 0.005

#### Scenario: 修复后 probe_time 显著降低
- **WHEN** 在重建的 COCO 100K 索引上运行 nprobe=200, queries=500
- **THEN** avg probe_time_ms < 0.5ms（修复前为 1.75ms）
