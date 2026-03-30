#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/io/npy_reader.h"

namespace vdb {
namespace io {

/// Row-major int32 array loaded from .ivecs (ground truth).
struct VecsArrayInt32 {
    std::vector<int32_t> data;   // row-major [rows x cols]
    uint32_t rows;
    uint32_t cols;
};

/// Load a .fvecs file (ANN-Benchmark float32 format).
/// Each record: [dim: int32][values: float[dim]], total 4 + dim*4 bytes.
/// Returns NpyArrayFloat for type compatibility with npy reader.
StatusOr<NpyArrayFloat> LoadFvecs(const std::string& path);

/// Load a .ivecs file (ANN-Benchmark int32 format, typically ground truth).
/// Each record: [dim: int32][values: int32[dim]], total 4 + dim*4 bytes.
StatusOr<VecsArrayInt32> LoadIvecs(const std::string& path);

/// Load vectors from any supported format, dispatching by extension.
/// Supports .fvecs and .npy.
StatusOr<NpyArrayFloat> LoadVectors(const std::string& path);

}  // namespace io
}  // namespace vdb
