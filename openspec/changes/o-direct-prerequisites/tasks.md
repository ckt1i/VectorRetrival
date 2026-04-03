## 1. .clu v8 4KB 对齐布局

- [x] 1.1 Writer: kFileVersion 从 7 改为 8，Open() 中写完 lookup table 后 pad 到 4KB 边界
- [x] 1.2 Writer: EndCluster() 中写完 mini-trailer 后 pad current_offset_ 到 4KB 边界
- [x] 1.3 Reader: Open() 支持 version 7 和 8，v8 时读完 lookup table 后跳过 pad 到 4KB 边界
- [x] 1.4 OverlapScheduler: SubmitClusterRead 中 read length round_up 到 4KB，ParseClusterBlock 仍用原始 block_size

## 2. Buffer 分配对齐

- [x] 2.1 OverlapScheduler: SubmitClusterRead 中 new uint8_t[] 改为 aligned_alloc(4096, ...)，释放改为 free()
- [x] 2.2 BufferPool: 内部分配改为 aligned_alloc(4096, ...)，释放改为 free()

## 3. Lookup table bulk pread

- [x] 3.1 Reader::Open() 中将逐字段 PreadValue 循环替换为单次 pread + 内存解析

## 4. Fixed Files

- [x] 4.1 IoUringReader 新增 RegisterFiles(const int*, count) 方法
- [x] 4.2 PrepRead 自动检测已注册 fd 并使用 IOSQE_FIXED_FILE（+ PrepReadFixed 显式接口）
- [x] 4.3 bench_e2e 初始化后调用 RegisterFiles 注册 clu_fd + dat_fd

## 5. 验证

- [x] 5.1 编译通过，运行 bench_e2e --cold --bits 4 --queries 500 验证功能正确性（recall一致，无crash）
