#pragma once

#include <algorithm>
#include <cstddef>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace simd {

VDB_FORCE_INLINE void ComputeCoarseIPScoresPackedScalar(
    const float* VDB_RESTRICT query,
    const float* VDB_RESTRICT packed_centroids,
    uint32_t nlist,
    uint32_t centroid_block,
    uint32_t num_dim_blocks,
    uint32_t vec_width,
    float* VDB_RESTRICT scores) {
    const size_t block_stride =
        static_cast<size_t>(num_dim_blocks) * centroid_block * vec_width;
    for (uint32_t cb = 0; cb < (nlist + centroid_block - 1) / centroid_block; ++cb) {
        float acc[8] = {};
        const float* block_ptr = packed_centroids + static_cast<size_t>(cb) * block_stride;
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const float* qv = query + static_cast<size_t>(db) * vec_width;
            const float* block = block_ptr + static_cast<size_t>(db) * centroid_block * vec_width;
            for (uint32_t lane = 0; lane < centroid_block; ++lane) {
                const float* cv = block + static_cast<size_t>(lane) * vec_width;
                for (uint32_t j = 0; j < vec_width; ++j) {
                    acc[lane] += qv[j] * cv[j];
                }
            }
        }
        const uint32_t lane_count = std::min<uint32_t>(centroid_block, nlist - cb * centroid_block);
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            scores[cb * centroid_block + lane] = -acc[lane];
        }
    }
}

#if defined(VDB_USE_AVX512)
namespace coarse_detail {
VDB_FORCE_INLINE float ReduceAdd512(__m512 v) {
    return _mm512_reduce_add_ps(v);
}
}  // namespace coarse_detail

VDB_FORCE_INLINE void ComputeCoarseIPScoresPacked(
    const float* VDB_RESTRICT query,
    const float* VDB_RESTRICT packed_centroids,
    uint32_t nlist,
    float* VDB_RESTRICT scores,
    uint32_t num_dim_blocks) {
    constexpr uint32_t kCentroidBlock = 8;
    constexpr uint32_t kVecWidth = 16;
    const size_t block_stride =
        static_cast<size_t>(num_dim_blocks) * kCentroidBlock * kVecWidth;
    const uint32_t num_centroid_blocks = (nlist + kCentroidBlock - 1) / kCentroidBlock;

    for (uint32_t cb = 0; cb < num_centroid_blocks; ++cb) {
        __m512 acc[kCentroidBlock];
        for (uint32_t lane = 0; lane < kCentroidBlock; ++lane) {
            acc[lane] = _mm512_setzero_ps();
        }
        const float* block_ptr = packed_centroids + static_cast<size_t>(cb) * block_stride;
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const __m512 qv = _mm512_loadu_ps(query + static_cast<size_t>(db) * kVecWidth);
            const float* block = block_ptr + static_cast<size_t>(db) * kCentroidBlock * kVecWidth;
            for (uint32_t lane = 0; lane < kCentroidBlock; ++lane) {
                const __m512 cv = _mm512_load_ps(block + static_cast<size_t>(lane) * kVecWidth);
                acc[lane] = _mm512_fmadd_ps(qv, cv, acc[lane]);
            }
        }
        const uint32_t lane_count = std::min<uint32_t>(kCentroidBlock, nlist - cb * kCentroidBlock);
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            scores[cb * kCentroidBlock + lane] = -coarse_detail::ReduceAdd512(acc[lane]);
        }
    }
}
#elif defined(VDB_USE_AVX2)
namespace coarse_detail {
VDB_FORCE_INLINE float ReduceAdd256(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}
}  // namespace coarse_detail

VDB_FORCE_INLINE void ComputeCoarseIPScoresPacked(
    const float* VDB_RESTRICT query,
    const float* VDB_RESTRICT packed_centroids,
    uint32_t nlist,
    float* VDB_RESTRICT scores,
    uint32_t num_dim_blocks) {
    constexpr uint32_t kCentroidBlock = 4;
    constexpr uint32_t kVecWidth = 8;
    const size_t block_stride =
        static_cast<size_t>(num_dim_blocks) * kCentroidBlock * kVecWidth;
    const uint32_t num_centroid_blocks = (nlist + kCentroidBlock - 1) / kCentroidBlock;

    for (uint32_t cb = 0; cb < num_centroid_blocks; ++cb) {
        __m256 acc[kCentroidBlock];
        for (uint32_t lane = 0; lane < kCentroidBlock; ++lane) {
            acc[lane] = _mm256_setzero_ps();
        }
        const float* block_ptr = packed_centroids + static_cast<size_t>(cb) * block_stride;
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const __m256 qv = _mm256_loadu_ps(query + static_cast<size_t>(db) * kVecWidth);
            const float* block = block_ptr + static_cast<size_t>(db) * kCentroidBlock * kVecWidth;
            for (uint32_t lane = 0; lane < kCentroidBlock; ++lane) {
                const __m256 cv = _mm256_load_ps(block + static_cast<size_t>(lane) * kVecWidth);
                acc[lane] = _mm256_fmadd_ps(qv, cv, acc[lane]);
            }
        }
        const uint32_t lane_count = std::min<uint32_t>(kCentroidBlock, nlist - cb * kCentroidBlock);
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            scores[cb * kCentroidBlock + lane] = -coarse_detail::ReduceAdd256(acc[lane]);
        }
    }
}
#else
VDB_FORCE_INLINE void ComputeCoarseIPScoresPacked(
    const float* VDB_RESTRICT query,
    const float* VDB_RESTRICT packed_centroids,
    uint32_t nlist,
    float* VDB_RESTRICT scores,
    uint32_t num_dim_blocks) {
    ComputeCoarseIPScoresPackedScalar(
        query, packed_centroids, nlist, 1u, num_dim_blocks, 1u, scores);
}
#endif

}  // namespace simd
}  // namespace vdb
