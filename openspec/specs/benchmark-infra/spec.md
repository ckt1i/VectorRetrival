# Spec: Benchmark Infrastructure

## 目录结构

- Benchmark 源文件位于 `benchmarks/` 顶层目录
- Unit test 源文件保留在 `tests/` 目录，不受影响

## 编译隔离

- `VDB_BUILD_BENCHMARKS=ON` 编译所有 benchmark targets
- `VDB_BUILD_BENCHMARKS=OFF`（默认）不编译任何 benchmark
- `VDB_BUILD_TESTS` 和 `VDB_BUILD_BENCHMARKS` 互相独立，可任意组合
- 支持 `cmake --build build --target <bench_name>` 单独编译指定 benchmark

## benchmarks/CMakeLists.txt

- 受 `VDB_BUILD_BENCHMARKS` 控制
- 每个 benchmark 独立 `add_executable` + `target_link_libraries`
- 不注册 `add_test()`（benchmark 不参与 ctest）

## 根 CMakeLists.txt 改动

- `VDB_BUILD_TESTS` 块内不再包含 bench_* targets
- 在 tests 块之后添加 `add_subdirectory(benchmarks)`
