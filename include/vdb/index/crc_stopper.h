#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vdb {
namespace index {

/// Calibration results from CRC offline calibration.
/// Used by both CrcCalibrator (offline) and CrcStopper (online).
struct CalibrationResults {
    float lamhat = 0.0f;       // λ̂: stopping threshold from Brent root-finding
    uint32_t kreg = 1;         // k_reg: RAPS free-probe allowance
    float reg_lambda = 0.0f;   // λ_reg: RAPS rank penalty coefficient
    float d_min = 0.0f;        // global min d_p (normalization lower bound)
    float d_max = 0.0f;        // global max d_p (normalization upper bound)
};

/// Online early-stop decision maker for CRC (Conformal Risk Control).
/// O(1) per call, no allocations, no external dependencies.
class CrcStopper {
 public:
    CrcStopper() = default;

    CrcStopper(const CalibrationResults& params, uint32_t nlist)
        : lamhat_(params.lamhat),
          kreg_(params.kreg),
          reg_lambda_(params.reg_lambda),
          d_min_(params.d_min) {
        float d_range = params.d_max - params.d_min;
        d_range_inv_ = (d_range > 0.0f) ? (1.0f / d_range) : 0.0f;

        // Numerical safety margin (* 1.2) to keep reg_score well below 1.0,
        // ensuring Brent root-finding has stable bracket conditions.
        // Must match the same formula used in CrcCalibrator::regularize_scores().
        float raw_max = 1.0f + reg_lambda_ * std::max(0, static_cast<int>(nlist) -
                                                           static_cast<int>(kreg_));
        max_reg_val_ = raw_max * 1.2f;
    }

    /// Decide whether to stop probing after the given number of clusters.
    /// @param probed_count  Number of clusters probed so far (1-based).
    /// @param current_kth_dist  Current k-th nearest distance in the top-k heap.
    ///                          Pass +inf when the heap is not yet full.
    /// @return true if probing should stop.
    bool ShouldStop(uint32_t probed_count, float current_kth_dist) const {
        // Normalize to nonconformity score in [0, 1].
        // When heap is not full (dist = +inf), nonconf clamps to 1.0,
        // making (1 - nonconf) = 0, so reg_score is small → no early stop.
        float nonconf = (current_kth_dist - d_min_) * d_range_inv_;
        nonconf = std::clamp(nonconf, 0.0f, 1.0f);

        // RAPS regularization: confidence + rank penalty.
        float E = (1.0f - nonconf) +
                  reg_lambda_ * std::max(0, static_cast<int>(probed_count) -
                                                static_cast<int>(kreg_));
        float reg_score = E / max_reg_val_;

        return reg_score > lamhat_;
    }

 private:
    float lamhat_ = 0.0f;
    uint32_t kreg_ = 1;
    float reg_lambda_ = 0.0f;
    float d_min_ = 0.0f;
    float d_range_inv_ = 0.0f;   // 1.0 / (d_max - d_min), precomputed
    float max_reg_val_ = 1.0f;   // (1 + reg_lambda * max(0, nlist - kreg)) * 1.2
};

}  // namespace index
}  // namespace vdb
