#include "vdb/index/cluster_prober.h"

#include <algorithm>
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
                           ProbeResultSink& sink,
                           ProbeStats& stats) const {
    // Packed codes occupy the first FastScanPackedSize(dim_) bytes of each block.
    // Norms follow immediately after.
    const uint32_t packed_sz = storage::FastScanPackedSize(dim_);

    for (uint32_t b = 0; b < pc.num_fastscan_blocks; ++b) {
        const uint32_t base_idx = b * 32;
        const uint32_t count = std::min(32u, pc.num_records - base_idx);

        const uint8_t* block_ptr  = pc.fastscan_blocks + static_cast<size_t>(b) * pc.fastscan_block_size;
        const float*   block_norms = reinterpret_cast<const float*>(block_ptr + packed_sz);

        // Stage 1a: batch-32 FastScan distance estimation
        alignas(64) float dists[32];
        estimator_.EstimateDistanceFastScan(pq, block_ptr, block_norms, count, dists);

        // Stage 1b: batch SafeOut classification via SIMD bitmask.
        // Bit v set  ⟺  dists[v] > dynamic_d_k + 2 * margin_factor * block_norms[v]
        uint32_t so_mask = simd::FastScanSafeOutMask(
            dists, block_norms, count, dynamic_d_k, margin_factor);
        stats.s1_safeout += static_cast<uint32_t>(__builtin_popcount(so_mask));

        // Stage 1c: iterate non-SafeOut lanes via ctz
        uint32_t lane_valid = (count >= 32u) ? 0xFFFFFFFFu : ((1u << count) - 1u);
        uint32_t maybe_in   = (~so_mask) & lane_valid;

        while (maybe_in) {
            const uint32_t j = static_cast<uint32_t>(__builtin_ctz(maybe_in));
            maybe_in &= maybe_in - 1u;

            const uint32_t global_idx  = base_idx + j;
            const float    est_dist_s1 = dists[j];
            const float    margin_s1   = margin_factor * block_norms[j];

            // Stage 1d: SafeIn vs Uncertain (SafeOut already removed)
            ResultClass rc_s1;
            if (est_dist_s1 < conann_.d_k() - 2.0f * margin_s1) {
                rc_s1 = ResultClass::SafeIn;
                ++stats.s1_safein;
            } else {
                rc_s1 = ResultClass::Uncertain;
                ++stats.s1_uncertain;
            }

            ResultClass rc_final = rc_s1;

            // Stage 2: ExRaBitQ re-classification for S1-Uncertain (bits > 1)
            if (rc_s1 == ResultClass::Uncertain && has_s2_ &&
                pc.exrabitq_entries != nullptr) {
                const uint8_t* ex_code = pc.ex_code(global_idx);
                const uint8_t* ex_sign = pc.ex_sign(global_idx, dim_);
                const float    xipn    = pc.xipnorm(global_idx, dim_);
                const float    norm_oc = block_norms[j];

                const float ip_raw = simd::IPExRaBitQ(
                    pq.rotated.data(), ex_code, ex_sign, dim_);
                const float ip_est = ip_raw * xipn;

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
            }

            const CandidateClass cls = (rc_final == ResultClass::SafeIn)
                ? CandidateClass::SafeIn
                : CandidateClass::Uncertain;

            sink.OnCandidate(global_idx,
                             pc.decoded_addresses[global_idx],
                             est_dist_s1,
                             cls);
        }
    }
}

}  // namespace index
}  // namespace vdb
