#include "vdb/index/cluster_prober.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "vdb/simd/fastscan.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/storage/pack_codes.h"

namespace vdb {
namespace index {

ClusterProber::ClusterProber(const ConANN& conann, Dim dim, uint8_t bits)
    : conann_(conann),
      dim_(dim),
      bits_(bits),
      has_s2_(bits > 1),
      margin_s2_divisor_(bits > 1 ? static_cast<float>(1u << (bits - 1)) : 1.0f),
      estimator_(dim, bits) {}

ClusterProber::~ClusterProber() = default;

void ClusterProber::Probe(const query::ParsedCluster& pc,
                           const rabitq::PreparedQuery& pq,
                           float margin_factor,
                           float dynamic_d_k,
                           bool enable_fine_grained_timing,
                           ProbeResultSink& sink,
                           ProbeStats& stats) const {
    const uint32_t packed_sz = storage::FastScanPackedSize(dim_);
    const float safein_threshold_base = conann_.d_k();

    for (uint32_t b = 0; b < pc.num_fastscan_blocks; ++b) {
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
                pq, block_ptr, block_norms, count, dists);
            stats.stage1_estimate_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - estimate_start).count();
        } else {
            estimator_.EstimateDistanceFastScan(pq, block_ptr, block_norms, count, dists);
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

        // Stage 1c: iterate non-SafeOut lanes via ctz
        uint32_t lane_valid = (count >= 32u) ? 0xFFFFFFFFu : ((1u << count) - 1u);
        uint32_t maybe_in   = (~so_mask) & lane_valid;

        while (maybe_in) {
            uint32_t j = 0;
            if (enable_fine_grained_timing) {
                auto iterate_start = std::chrono::steady_clock::now();
                const uint32_t current_mask = maybe_in;
                j = static_cast<uint32_t>(__builtin_ctz(current_mask));
                maybe_in = current_mask & (current_mask - 1u);
                stats.stage1_iterate_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - iterate_start).count();
            } else {
                const uint32_t current_mask = maybe_in;
                j = static_cast<uint32_t>(__builtin_ctz(current_mask));
                maybe_in = current_mask & (current_mask - 1u);
            }

            const uint32_t global_idx  = base_idx + j;
            const float    est_dist_s1 = dists[j];
            const float    margin_s1   = margin_factor * block_norms[j];

            // Stage 1d: SafeIn vs Uncertain (SafeOut already removed)
            ResultClass rc_s1;
            if (enable_fine_grained_timing) {
                auto classify_start = std::chrono::steady_clock::now();
                if (est_dist_s1 < safein_threshold_base - 2.0f * margin_s1) {
                    rc_s1 = ResultClass::SafeIn;
                    ++stats.s1_safein;
                } else {
                    rc_s1 = ResultClass::Uncertain;
                    ++stats.s1_uncertain;
                }
                stats.stage1_classify_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - classify_start).count();
            } else {
                if (est_dist_s1 < safein_threshold_base - 2.0f * margin_s1) {
                    rc_s1 = ResultClass::SafeIn;
                    ++stats.s1_safein;
                } else {
                    rc_s1 = ResultClass::Uncertain;
                    ++stats.s1_uncertain;
                }
            }

            ResultClass rc_final = rc_s1;

            // Stage 2: ExRaBitQ re-classification for S1-Uncertain (bits > 1)
            if (rc_s1 == ResultClass::Uncertain && has_s2_ &&
                pc.exrabitq_entries != nullptr) {
                auto stage2_start = std::chrono::steady_clock::now();
                const query::ParsedCluster::ExRaBitQView ex_view =
                    pc.exrabitq_view(global_idx, dim_);
                const float    norm_oc = block_norms[j];

                const float ip_raw = simd::IPExRaBitQ(
                    pq.rotated.data(), ex_view.code_abs, ex_view.sign, dim_);
                const float ip_est = ip_raw * ex_view.xipnorm;

                float est_dist_s2 = norm_oc * norm_oc + pq.norm_qc_sq
                                  - 2.0f * norm_oc * pq.norm_qc * ip_est;
                est_dist_s2 = std::max(est_dist_s2, 0.0f);

                const float margin_s2 = margin_s1 / margin_s2_divisor_;
                const ResultClass rc_s2 =
                    conann_.ClassifyAdaptive(est_dist_s2, margin_s2, dynamic_d_k);

                if (rc_s2 == ResultClass::SafeIn) {
                    ++stats.s2_safein;
                } else if (rc_s2 == ResultClass::SafeOut) {
                    ++stats.s2_safeout;
                    continue;  // S2 SafeOut: skip candidate
                } else {
                    ++stats.s2_uncertain;
                }
                rc_final = rc_s2;
                if (enable_fine_grained_timing) {
                    double stage2_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - stage2_start).count();
                    stats.stage2_ms += stage2_ms;
                }
            }

            batch.global_idx[batch.count] = global_idx;
            batch.est_dist[batch.count] = est_dist_s1;
            batch.cls[batch.count] = (rc_final == ResultClass::SafeIn)
                ? CandidateClass::SafeIn
                : CandidateClass::Uncertain;
            batch.count++;
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
