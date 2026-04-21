#include "vdb/index/cluster_prober.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "vdb/simd/fastscan.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/storage/pack_codes.h"

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace index {

namespace {

constexpr double kLowOverheadStage2Weight = 1.0;
constexpr uint32_t kStage2BatchSize = 8;

VDB_FORCE_INLINE uint32_t LaneMaskForCount(uint32_t count) {
    return (count >= 32u) ? 0xFFFFFFFFu : ((1u << count) - 1u);
}

VDB_FORCE_INLINE uint32_t Stage1SafeInMask(const float* dists,
                                           const float* block_norms,
                                           uint32_t count,
                                           float safein_threshold_base,
                                           float margin_factor) {
    const float threshold_mul = 2.0f * margin_factor;
#if defined(VDB_USE_AVX512)
    __m512 vbase = _mm512_set1_ps(safein_threshold_base);
    __m512 vmul = _mm512_set1_ps(threshold_mul);
    __m512 vd = _mm512_loadu_ps(dists);
    __m512 vn = _mm512_loadu_ps(block_norms);
    __m512 vth = _mm512_sub_ps(vbase, _mm512_mul_ps(vmul, vn));
    uint32_t mask = static_cast<uint32_t>(_mm512_cmp_ps_mask(vd, vth, _CMP_LT_OQ));
    return mask & LaneMaskForCount(count);
#elif defined(VDB_USE_AVX2)
    __m256 vbase = _mm256_set1_ps(safein_threshold_base);
    __m256 vmul = _mm256_set1_ps(threshold_mul);
    __m256 vd0 = _mm256_loadu_ps(dists);
    __m256 vn0 = _mm256_loadu_ps(block_norms);
    __m256 vt0 = _mm256_sub_ps(vbase, _mm256_mul_ps(vmul, vn0));
    uint32_t mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(vd0, vt0, _CMP_LT_OQ)));
    if (count > 8) {
        __m256 vd1 = _mm256_loadu_ps(dists + 8);
        __m256 vn1 = _mm256_loadu_ps(block_norms + 8);
        __m256 vt1 = _mm256_sub_ps(vbase, _mm256_mul_ps(vmul, vn1));
        mask |= static_cast<uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(vd1, vt1, _CMP_LT_OQ))) << 8;
    }
    if (count > 16) {
        __m256 vd2 = _mm256_loadu_ps(dists + 16);
        __m256 vn2 = _mm256_loadu_ps(block_norms + 16);
        __m256 vt2 = _mm256_sub_ps(vbase, _mm256_mul_ps(vmul, vn2));
        mask |= static_cast<uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(vd2, vt2, _CMP_LT_OQ))) << 16;
    }
    if (count > 24) {
        __m256 vd3 = _mm256_loadu_ps(dists + 24);
        __m256 vn3 = _mm256_loadu_ps(block_norms + 24);
        __m256 vt3 = _mm256_sub_ps(vbase, _mm256_mul_ps(vmul, vn3));
        mask |= static_cast<uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(vd3, vt3, _CMP_LT_OQ))) << 24;
    }
    return mask & LaneMaskForCount(count);
#else
    uint32_t mask = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float threshold = safein_threshold_base - threshold_mul * block_norms[i];
        if (dists[i] < threshold) {
            mask |= (1u << i);
        }
    }
    return mask;
#endif
}

VDB_FORCE_INLINE uint32_t CompactMaskToIndices(uint32_t mask,
                                               uint32_t* VDB_RESTRICT out_idx) {
    uint32_t n = 0;
    while (mask) {
        const uint32_t lane = static_cast<uint32_t>(__builtin_ctz(mask));
        out_idx[n++] = lane;
        mask &= (mask - 1u);
    }
    return n;
}

}  // namespace

ClusterProber::ClusterProber(const ConANN& conann, Dim dim, uint8_t bits)
    : conann_(conann),
      dim_(dim),
      bits_(bits),
      has_s2_(bits > 1),
      margin_s2_divisor_(bits > 1 ? static_cast<float>(1u << (bits - 1)) : 1.0f),
      estimator_(dim, bits) {}

ClusterProber::~ClusterProber() = default;

void ClusterProber::Probe(const query::ParsedCluster& pc,
                           const rabitq::PreparedClusterQueryView& view,
                           float margin_factor,
                           float dynamic_d_k,
                           bool enable_fine_grained_timing,
                           ProbeResultSink& sink,
                           ProbeStats& stats) const {
    const rabitq::PreparedQuery& pq = *view.prepared;
    const uint32_t packed_sz = storage::FastScanPackedSize(dim_);
    const float safein_threshold_base = conann_.d_k();

    for (uint32_t b = 0; b < pc.num_fastscan_blocks; ++b) {
        ++stats.num_stage1_blocks;
        CandidateBatch batch;
        const uint32_t base_idx = b * 32;
        const uint32_t count = std::min(32u, pc.num_records - base_idx);

        const uint8_t* block_ptr =
            pc.fastscan_blocks + static_cast<size_t>(b) * pc.fastscan_block_size;
        const float* block_norms =
            reinterpret_cast<const float*>(block_ptr + packed_sz);

        // Stage 1a: batch-32 FastScan distance estimation
        alignas(64) float dists[32];
        if (enable_fine_grained_timing) {
            auto estimate_start = std::chrono::steady_clock::now();
            estimator_.EstimateDistanceFastScan(
                view, block_ptr, block_norms, count, dists);
            stats.stage1_estimate_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - estimate_start).count();
        } else {
            estimator_.EstimateDistanceFastScan(view, block_ptr, block_norms, count, dists);
        }

        // Stage 1b: batch SafeOut classification via SIMD bitmask.
        // Bit v set  ⟺  dists[v] > dynamic_d_k + 2 * margin_factor * block_norms[v]
        uint32_t so_mask = 0;
        if (enable_fine_grained_timing) {
            auto mask_start = std::chrono::steady_clock::now();
            so_mask = simd::FastScanSafeOutMask(
                dists, block_norms, count, dynamic_d_k, margin_factor);
            stats.stage1_mask_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - mask_start).count();
        } else {
            so_mask = simd::FastScanSafeOutMask(
                dists, block_norms, count, dynamic_d_k, margin_factor);
        }
        stats.s1_safeout += static_cast<uint32_t>(__builtin_popcount(so_mask));

        // Stage 1c/1d: compact survivors and classify them in mask batches.
        const uint32_t lane_valid = LaneMaskForCount(count);
        const uint32_t maybe_in = (~so_mask) & lane_valid;
        const uint32_t safein_mask = Stage1SafeInMask(
            dists, block_norms, count, safein_threshold_base, margin_factor) & maybe_in;
        const uint32_t uncertain_mask = maybe_in & ~safein_mask;

        uint32_t survivor_lanes[32];
        uint32_t survivor_count = 0;
        uint32_t safein_count = 0;
        uint32_t uncertain_count = 0;
        if (enable_fine_grained_timing) {
            auto iterate_start = std::chrono::steady_clock::now();
            survivor_count = CompactMaskToIndices(maybe_in, survivor_lanes);
            safein_count = static_cast<uint32_t>(__builtin_popcount(safein_mask));
            uncertain_count = static_cast<uint32_t>(__builtin_popcount(uncertain_mask));
            stats.stage1_iterate_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - iterate_start).count();
        } else {
            survivor_count = CompactMaskToIndices(maybe_in, survivor_lanes);
            safein_count = static_cast<uint32_t>(__builtin_popcount(safein_mask));
            uncertain_count = static_cast<uint32_t>(__builtin_popcount(uncertain_mask));
        }

        if (enable_fine_grained_timing) {
            auto classify_start = std::chrono::steady_clock::now();
            stats.s1_safein += safein_count;
            stats.s1_uncertain += uncertain_count;
            stats.stage1_classify_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - classify_start).count();
        } else {
            stats.s1_safein += safein_count;
            stats.s1_uncertain += uncertain_count;
        }

        struct Stage2ScratchEntry {
            uint32_t global_idx = 0;
            float est_dist_s1 = 0.0f;
            float margin_s1 = 0.0f;
            float norm_oc = 0.0f;
            const uint8_t* code_abs = nullptr;
            const uint8_t* sign = nullptr;
            float xipnorm = 0.0f;
            bool sign_packed = false;
        };
        Stage2ScratchEntry stage2_candidates[32];
        uint32_t stage2_candidate_count = 0;

        for (uint32_t s = 0; s < survivor_count; ++s) {
            const uint32_t j = survivor_lanes[s];
            const uint32_t global_idx = base_idx + j;
            const float est_dist_s1 = dists[j];
            const float margin_s1 = margin_factor * block_norms[j];
            ResultClass rc_final = ((safein_mask >> j) & 1u) != 0
                ? ResultClass::SafeIn
                : ResultClass::Uncertain;

            // Stage 2: collect S1-Uncertain candidates and batch packed-sign boosting.
            if (rc_final == ResultClass::Uncertain && has_s2_ &&
                pc.exrabitq_entries != nullptr) {
                const query::ParsedCluster::ExRaBitQView ex_view =
                    pc.exrabitq_view(global_idx, dim_);
                Stage2ScratchEntry& entry = stage2_candidates[stage2_candidate_count++];
                entry.global_idx = global_idx;
                entry.est_dist_s1 = est_dist_s1;
                entry.margin_s1 = margin_s1;
                entry.norm_oc = block_norms[j];
                entry.code_abs = ex_view.code_abs;
                entry.sign = ex_view.sign;
                entry.xipnorm = ex_view.xipnorm;
                entry.sign_packed = ex_view.sign_packed;
                continue;
            }

            batch.global_idx[batch.count] = global_idx;
            batch.est_dist[batch.count] = est_dist_s1;
            batch.cls[batch.count] = (rc_final == ResultClass::SafeIn)
                ? CandidateClass::SafeIn
                : CandidateClass::Uncertain;
            batch.count++;
        }

        if (stage2_candidate_count > 0) {
            stats.num_stage2_candidates += stage2_candidate_count;

            auto stage2_start = enable_fine_grained_timing
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const float* const rotated_query = pq.rotated.data();

            for (uint32_t base = 0; base < stage2_candidate_count; base += kStage2BatchSize) {
                const uint32_t chunk =
                    std::min(kStage2BatchSize, stage2_candidate_count - base);

                bool all_packed = true;
                for (uint32_t i = 0; i < chunk; ++i) {
                    if (!stage2_candidates[base + i].sign_packed) {
                        all_packed = false;
                        break;
                    }
                }

                alignas(64) float ip_raw_batch[kStage2BatchSize] = {};
                if (all_packed) {
                    const uint8_t* code_abs_ptrs[kStage2BatchSize] = {};
                    const uint8_t* sign_ptrs[kStage2BatchSize] = {};
                    for (uint32_t i = 0; i < chunk; ++i) {
                        code_abs_ptrs[i] = stage2_candidates[base + i].code_abs;
                        sign_ptrs[i] = stage2_candidates[base + i].sign;
                    }
                    simd::IPExRaBitQBatchPackedSign(
                        rotated_query, code_abs_ptrs, sign_ptrs, chunk, dim_, ip_raw_batch);
                } else {
                    for (uint32_t i = 0; i < chunk; ++i) {
                        const Stage2ScratchEntry& entry = stage2_candidates[base + i];
                        ip_raw_batch[i] = simd::IPExRaBitQ(
                            rotated_query, entry.code_abs, entry.sign, entry.sign_packed, dim_);
                    }
                }

                for (uint32_t i = 0; i < chunk; ++i) {
                    const Stage2ScratchEntry& entry = stage2_candidates[base + i];
                    const float ip_est = ip_raw_batch[i] * entry.xipnorm;

                    float est_dist_s2 = entry.norm_oc * entry.norm_oc + pq.norm_qc_sq
                                      - 2.0f * entry.norm_oc * pq.norm_qc * ip_est;
                    est_dist_s2 = std::max(est_dist_s2, 0.0f);

                    const float margin_s2 = entry.margin_s1 / margin_s2_divisor_;
                    const ResultClass rc_s2 =
                        conann_.ClassifyAdaptive(est_dist_s2, margin_s2, dynamic_d_k);

                    if (rc_s2 == ResultClass::SafeIn) {
                        ++stats.s2_safein;
                    } else if (rc_s2 == ResultClass::SafeOut) {
                        ++stats.s2_safeout;
                        continue;
                    } else {
                        ++stats.s2_uncertain;
                    }

                    batch.global_idx[batch.count] = entry.global_idx;
                    batch.est_dist[batch.count] = entry.est_dist_s1;
                    batch.cls[batch.count] = (rc_s2 == ResultClass::SafeIn)
                        ? CandidateClass::SafeIn
                        : CandidateClass::Uncertain;
                    batch.count++;
                }
            }

            if (enable_fine_grained_timing) {
                stats.stage2_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - stage2_start).count();
            }
        }

        if (batch.count > 0) {
            pc.DecodeAddressBatch(batch.global_idx, batch.count, batch.decoded_addr);
            sink.OnCandidates(batch);
        }
    }

    if (enable_fine_grained_timing) {
        stats.stage1_ms = stats.stage1_estimate_ms +
                          stats.stage1_mask_ms +
                          stats.stage1_iterate_ms +
                          stats.stage1_classify_ms;
    }
}

}  // namespace index
}  // namespace vdb
