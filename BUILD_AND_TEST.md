# VDB VectorRetrival — 编译与测试指南

> 面向在新服务器上部署编译环境和运行全部测试的场景

---

## 1. 系统要求

### 操作系统

- Ubuntu 22.04+ / Debian 12+（其他 Linux 发行版理论可行但未测试）
- Linux 内核 >= 5.1（io_uring 支持）

### 编译器

- GCC >= 11（当前开发环境: GCC 13.3.0）
- 需要 C++17 支持

### CPU 指令集

| 指令集 | 用途 | 必须？ |
|--------|------|--------|
| AVX2 + FMA | 基础 SIMD（L2距离、popcount、内积） | 是（默认开启） |
| AVX-512F/BW/DQ/VL | 高级 SIMD（FastScan VPSHUFB） | 可选（cmake 开关） |
| AVX-512 VPOPCNTDQ | 硬件 popcount | 可选（仅 AVX-512 模式） |

检查 CPU 支持：
```bash
grep -o 'avx2' /proc/cpuinfo | head -1           # 必须有
grep -o 'avx512f' /proc/cpuinfo | head -1         # 可选
grep -o 'avx512_vpopcntdq' /proc/cpuinfo | head -1 # 可选
```

如果目标机器没有 AVX-512，用 AVX2 模式编译即可（默认）。如果连 AVX2 都没有，设置 `-DVDB_USE_AVX2=OFF` 使用纯标量 fallback。

### CMake

- >= 3.16（核心项目）
- >= 3.26（SuperKMeans 子项目要求，如果 cmake < 3.26 需升级）

```bash
cmake --version
```

---

## 2. 依赖安装

### Ubuntu/Debian 一键安装

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  libgsl-dev \
  liburing-dev \
  libopenblas-dev \
  git
```

### 依赖说明

| 依赖 | 用途 | 安装方式 |
|------|------|----------|
| **GSL** (libgsl-dev) | CRC 校准数学运算 | `apt install libgsl-dev` |
| **liburing** (liburing-dev) | io_uring 异步 I/O | `apt install liburing-dev` |
| **OpenBLAS** (libopenblas-dev) | SuperKMeans BLAS 计算 | `apt install libopenblas-dev` |
| **FlatBuffers** | schema 序列化 | 自动 FetchContent 下载（无需手动安装） |
| **GoogleTest** | 单元测试框架 | 自动 FetchContent 下载 |
| **Eigen 3.4** | SuperKMeans 内部使用 | 已 bundled 在 `thrid-party/SuperKMeans/extern/Eigen/` |
| **OpenMP** | SuperKMeans 多线程 | GCC 自带（libgomp） |

### 验证依赖

```bash
pkg-config --libs gsl          # 应输出: -lgsl -lgslcblas -lm
dpkg -l | grep liburing-dev    # 应有 liburing-dev
dpkg -l | grep libopenblas-dev # 应有 libopenblas-dev
```

---

## 3. 编译

### 基础编译（AVX2，单元测试 + Benchmark）

```bash
cd /path/to/VectorRetrival
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DVDB_BUILD_TESTS=ON \
         -DVDB_BUILD_BENCHMARKS=ON
cmake --build . -j$(nproc)
```

### AVX-512 编译（需要 CPU 支持）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DVDB_BUILD_TESTS=ON \
         -DVDB_BUILD_BENCHMARKS=ON \
         -DVDB_USE_AVX512=ON
cmake --build . -j$(nproc)
```

### 仅编译单测（不编译 Benchmark）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DVDB_BUILD_TESTS=ON \
         -DVDB_BUILD_BENCHMARKS=OFF
cmake --build . -j$(nproc)
```

### 编译产物

```
build/
├── libvdb_*.a          ← 9 个静态库
├── test_*              ← 37 个测试可执行文件
└── benchmarks/
    └── bench_*         ← 8 个 benchmark 可执行文件
```

---

## 4. 运行单元测试

### 全部运行

```bash
cd build
ctest --output-on-failure -j$(nproc)
```

### 按模块运行

```bash
# RaBitQ 核心
./test_rabitq_encoder
./test_rabitq_estimator
./test_rabitq_rotation

# SIMD
./test_distance_l2
./test_popcount
./test_bit_unpack
./test_prefix_sum
./test_transpose

# 存储层
./test_cluster_store
./test_data_file
./test_segment
./test_column_store
./test_column_chunk
./test_address_column

# 索引
./test_ivf_builder
./test_ivf_index
./test_conann
./test_crc_calibrator
./test_crc_stopper

# 查询流水线
./test_overlap_scheduler
./test_rerank_consumer
./test_result_collector
./test_early_stop

# I/O
./test_io_uring_reader
./test_pread_fallback_reader
./test_npy_reader
./test_vecs_reader
./test_jsonl_reader

# 其他
./test_status
./test_types
./test_schema
./test_record
./test_distance
./test_aligned_alloc
./test_buffer_pool
./test_bitpack_codec
./test_dict_codec
./test_payload_pipeline
```

### 预期结果

全部 PASSED，无 FAILED。如果 `test_io_uring_reader` 失败，可能是内核版本太低（< 5.1）或 `/proc/sys/kernel/io_uring_group` 权限问题。

---

## 5. 数据集

### 数据集一览

| 数据集 | N | D | 大小 | 格式 | 用途 |
|--------|---|---|------|------|------|
| deep1m | 1,000,000 | 96 | 558M | .fvecs/.ivecs | 大规模 ANN 评测 |
| coco_1k | ~1,000 | 512 | 162M | .npy + images | 快速冒烟测试 |
| coco_5k | ~5,000 | 512 | 810M | .npy + images | 中等规模测试 |
| coco_10k | ~10,000 | 512 | 1.6G | .npy + images | |
| coco_100k | ~100,000 | 512 | 16G | .npy + images | E2E 主要评测数据集 |
| coco_1m | ~1,000,000 | 512 | 19G | .npy + images | 大规模高维评测 |

### 数据目录结构

```
/home/zcq/VDB/data/
├── deep1m/
│   ├── deep1m_base.fvecs              ← 1M × 96 维基底向量
│   ├── deep1m_query.fvecs             ← 10K × 96 维查询向量
│   ├── deep1m_groundtruth.ivecs       ← 10K × 1000 真值
│   ├── deep1m_centroid_4096.fvecs     ← 预计算 faiss K=4096 质心
│   └── deep1m_cluster_id_4096.ivecs   ← 预计算分配
│
├── coco_100k/
│   ├── image_embeddings.npy           ← 100K × 512 维图像嵌入
│   ├── query_embeddings.npy           ← 查询嵌入
│   ├── image_ids.npy                  ← 图像 ID
│   ├── query_ids.npy                  ← 查询 ID
│   ├── images/                        ← 原始图片（bench_e2e payload）
│   ├── metadata.jsonl                 ← 元数据
│   ├── queries.jsonl                  ← 查询信息
│   └── summary.json                   ← 数据集摘要
│
└── coco_1k/ (结构同上，规模更小)
```

### 新服务器上的数据迁移

最小集合（足以运行全部 benchmark）：
```bash
# 必须
rsync -avP /home/zcq/VDB/data/deep1m/ new_server:/path/to/data/deep1m/
rsync -avP /home/zcq/VDB/data/coco_1k/ new_server:/path/to/data/coco_1k/

# 推荐（E2E 评测）
rsync -avP /home/zcq/VDB/data/coco_100k/ new_server:/path/to/data/coco_100k/
```

---

## 6. Benchmark 运行指南

### 6.1 bench_rabitq_accuracy — RaBitQ 距离精度评测

```bash
# 快速测试（coco_1k, ~10秒）
./benchmarks/bench_rabitq_accuracy \
  --dataset /path/to/data/coco_1k \
  --bits 4 --queries 10 --topk 10

# 标准测试（deep1m, 预计算聚类, ~2分钟）
./benchmarks/bench_rabitq_accuracy \
  --base /path/to/data/deep1m/deep1m_base.fvecs \
  --query /path/to/data/deep1m/deep1m_query.fvecs \
  --centroids /path/to/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /path/to/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --bits 4 --queries 10 --topk 10 --nlist 4096
```

关键参数: `--bits 1|2|4`, `--nlist`, `--queries`, `--topk`, `--p-for-eps-ip`

### 6.2 bench_vector_search — 向量搜索评测

```bash
# deep1m 标准测试
./benchmarks/bench_vector_search \
  --base /path/to/data/deep1m/deep1m_base.fvecs \
  --query /path/to/data/deep1m/deep1m_query.fvecs \
  --gt /path/to/data/deep1m/deep1m_groundtruth.ivecs \
  --centroids /path/to/data/deep1m/deep1m_centroid_4096.fvecs \
  --assignments /path/to/data/deep1m/deep1m_cluster_id_4096.ivecs \
  --nlist 4096 --nprobe 50 --topk 10 --bits 4 --queries 100 --early-stop 0
```

关键参数: `--nprobe`, `--bits`, `--crc 0|1`, `--early-stop 0|1`, `--outdir`（输出 JSON）

### 6.3 bench_e2e — 端到端评测（含索引构建 + 查询 + Payload）

```bash
# coco_1k 快速冒烟
./benchmarks/bench_e2e \
  --dataset /path/to/data/coco_1k \
  --bits 4 --nlist 32 --nprobe 8 --topk 10

# coco_100k 完整评测
./benchmarks/bench_e2e \
  --dataset /path/to/data/coco_100k \
  --bits 4 --nlist 512 --nprobe 32 --topk 10 --queries 100
```

bench_e2e 会自动执行: 构建索引 → 计算 GT → CRC 校准 → 查询 → 报告 recall + latency

### 6.4 其他 Benchmark

```bash
# ConANN 分类准确率
./benchmarks/bench_conann_recall --dataset /path/to/data/coco_1k

# CRC 校准评测
./benchmarks/bench_conann_crc --dataset /path/to/data/coco_1k

# IVF 聚类质量
./benchmarks/bench_ivf_quality --N 10000 --dim 128 --nlist 32

# RaBitQ 诊断
./benchmarks/bench_rabitq_diagnostic --dataset /path/to/data/coco_1k
```

---

## 7. 预期性能参考值

### deep1m (N=1M, D=96, K=4096, bits=4)

| 指标 | bench_rabitq_accuracy | bench_vector_search |
|------|----------------------|---------------------|
| Stage 1 MAE | 0.105 | — |
| Stage 2 MAE | 0.006 | — |
| Stage 2 recall@10 | 0.96 | — |
| recall@10 (nprobe=50) | — | 0.9730 |
| False SafeOut | — | 0 |
| latency avg | — | ~2.9ms |

### coco_100k (N=100K, D=512)

- SuperKMeans 聚类 (K=512): ~4.6 秒
- bench_e2e: recall@10 >= 0.95（取决于 nprobe）

---

## 8. 常见问题

### cmake 找不到 GSL

```
Could NOT find GSL (missing: GSL_LIBRARY)
```

解决: `sudo apt install libgsl-dev`

### SuperKMeans 编译错误（cmake 版本太低）

```
CMake Error at thrid-party/SuperKMeans/CMakeLists.txt:1:
  CMake 3.26 or higher is required.
```

解决: 升级 cmake 到 3.26+

### io_uring 测试失败

```
test_io_uring_reader: FAILED
```

可能原因:
- 内核 < 5.1: `uname -r` 检查
- 权限问题: `cat /proc/sys/kernel/io_uring_disabled`（应为 0）

### AVX-512 编译但运行 SIGILL

目标 CPU 不支持 AVX-512。改用 AVX2 编译:
```bash
cmake .. -DVDB_USE_AVX512=OFF -DVDB_USE_AVX2=ON
```

### MKL 导致 ConANN 分类全部 Uncertain（eps_ip 异常偏大）

**现象**: `bench_vector_search` 或 `bench_e2e` 运行时，Stage 1/Stage 2 的 SafeOut 均为 0%，所有向量都被分类为 Uncertain。日志中 `eps_ip` 比预期大 2-3 倍（如 0.21 vs 正常的 0.09）。

**原因**: 系统安装了 Intel MKL（`libmkl_intel_lp64.so` 等），SuperKMeans 的 Eigen 自动链接 MKL 作为 BLAS backend。MKL 与 OpenBLAS 的浮点累积误差路径不同，导致 KMeans 收敛到不同的局部最优。在 unit-norm 数据集（如 CLIP embeddings）上，残差方向对 centroid 位置极度敏感，聚类差异被放大为 eps_ip 的显著偏移。

**诊断**:
```bash
# 检查是否链接了 MKL
ldd build/benchmarks/bench_vector_search | grep mkl
# 如果输出 libmkl_*.so → MKL 被启用了
```

**解决**: 重新配置 cmake 时禁用 MKL，强制使用 OpenBLAS:
```bash
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DVDB_BUILD_BENCHMARKS=ON \
         -DMKL_DIR=/dev/null \
         -DBLA_VENDOR=OpenBLAS
cmake --build . -j$(nproc)
```

### FetchContent 下载失败（网络问题）

FlatBuffers 和 GoogleTest 需要从 GitHub 下载。如果服务器无外网:
1. 在有网络的机器上先编译一次，让 `_deps/` 缓存下来
2. 拷贝整个 `build/_deps/` 到新服务器
3. 或手动下载 tarball 放到 `_deps/` 对应目录

---

## 9. 第三方库说明

```
thrid-party/
├── SuperKMeans/          ← cmake add_subdirectory 集成，header-only
│   ├── include/          ← 头文件
│   └── extern/Eigen/     ← bundled Eigen 3.4.0
│
└── Extended-RaBitQ/      ← 仅供算法参考，不参与编译
    ├── bin/              ← 独立可执行文件（需单独编译）
    └── inc/third/        ← 独立的 Eigen + hnswlib
```

Extended-RaBitQ 如需单独编译（参考用途）:
```bash
cd thrid-party/Extended-RaBitQ
mkdir -p build && cd build
cmake .. && make -j$(nproc)
# 产物在 ../bin/: create_index, test_search
```
