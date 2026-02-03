# VectorDB

A high-performance Vector Search Engine + Columnar Payload Store.

## Features

- **IVF+PQ Indexing**: Inverted File Index with Product Quantization for approximate nearest neighbor search
- **Columnar Storage**: FlatBuffers-based column-first storage with per-column adaptive chunking
- **SIMD Acceleration**: AVX2/AVX-512 optimized distance computation
- **WAL + Snapshot**: Write-ahead logging with snapshot isolation for durability

## Building

### Prerequisites

- CMake 3.16+
- C++17 compatible compiler (GCC 9+, Clang 10+)
- Git

### Build Instructions

```bash
# Clone the repository
git clone <repo-url>
cd VectorAndColumn

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `VDB_BUILD_TESTS` | ON | Build unit tests |
| `VDB_BUILD_BENCHMARKS` | ON | Build benchmarks |
| `VDB_BUILD_TOOLS` | ON | Build CLI tools |
| `VDB_USE_BLAS` | ON | Enable OpenBLAS/MKL for k-means |
| `VDB_USE_MIMALLOC` | ON | Use mimalloc allocator |
| `VDB_USE_AVX2` | ON | Enable AVX2 SIMD |
| `VDB_USE_AVX512` | OFF | Enable AVX-512 SIMD |

## Project Structure

```
vectordb/
├── CMakeLists.txt              # Root CMake configuration
├── cmake/                      # CMake modules
│   └── FlatBuffersGenerate.cmake
├── schema/                     # FlatBuffers schema definitions
│   ├── columns.fbs            # Column store metadata
│   └── segment_meta.fbs       # Segment metadata
├── include/vdb/               # Public headers
│   ├── common/                # Basic types, status, allocation
│   ├── dist/                  # Distance computation
│   ├── storage/               # I/O, caching, WAL
│   ├── index/                 # IVF, PQ, Segment
│   ├── columns/               # Column store, encoders
│   ├── query/                 # Query engine, TopK
│   └── payload/               # Payload API
├── src/                       # Implementation
├── tools/                     # CLI utilities
├── tests/                     # Unit tests
└── benchmark/                 # Performance tests
```

## License

TBD
