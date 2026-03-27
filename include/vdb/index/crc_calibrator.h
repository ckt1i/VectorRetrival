#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/crc_stopper.h"

namespace vdb {
namespace rabitq { class RotationMatrix; }
namespace index {

/// Per-cluster vector data for CRC calibration.
/// The calibrator needs raw vectors to compute exact L2 distances,
/// and optionally RaBitQ codes for estimate-based calibration.
struct ClusterData {
    const float* vectors;    // contiguous [count × dim] row-major
    const uint32_t* ids;     // global vector IDs
    uint32_t count;          // number of vectors in this cluster
    const uint8_t* codes_block = nullptr;  // RaBitQ encoded block (optional)
    uint32_t code_entry_size = 0;           // bytes per entry in codes_block
};

/// Offline CRC calibrator.
/// Computes CalibrationResults from a calibration query set + IVF clusters.
class CrcCalibrator {
 public:
    struct Config {
        float alpha = 0.1f;          // target FNR upper bound
        uint32_t top_k = 10;         // k for top-k search
        float calib_ratio = 0.5f;    // fraction of queries for calibration
        float tune_ratio = 0.1f;     // fraction for tuning (k_reg, λ_reg)
        uint64_t seed = 42;          // random seed for query split
    };

    struct EvalResults {
        float actual_fnr = 0.0f;     // measured FNR on test set
        float avg_probed = 0.0f;     // average clusters probed on test set
        float recall_at_1 = 0.0f;
        float recall_at_5 = 0.0f;
        float recall_at_10 = 0.0f;
        uint32_t test_size = 0;
    };

    /// Main calibration entry point.
    /// @param config       Calibration hyperparameters.
    /// @param queries      Query vectors [num_queries × dim], row-major.
    /// @param num_queries  Number of calibration queries.
    /// @param dim          Vector dimensionality.
    /// @param centroids    Cluster centroids [nlist × dim], row-major.
    /// @param nlist        Number of clusters.
    /// @param clusters     Per-cluster vector data (size = nlist).
    /// @param ground_truth Per-query ground truth top-k IDs (size = num_queries).
    /// @return Calibration parameters + test set evaluation results.
    static std::pair<CalibrationResults, EvalResults> Calibrate(
        const Config& config,
        const float* queries, uint32_t num_queries, Dim dim,
        const float* centroids, uint32_t nlist,
        const std::vector<ClusterData>& clusters,
        const std::vector<std::vector<uint32_t>>& ground_truth);

    /// Calibrate using RaBitQ estimated distances for scoring.
    ///
    /// Same as Calibrate() but the scoring heap uses RaBitQ estimates
    /// instead of exact L2. This ensures the calibrated d_min/d_max/lamhat
    /// live in the same distance space as online CrcStopper inputs.
    /// Ground truth is still based on exact L2 (externally provided).
    ///
    /// Requires: ClusterData::codes_block and code_entry_size must be set.
    static std::pair<CalibrationResults, EvalResults> CalibrateWithRaBitQ(
        const Config& config,
        const float* queries, uint32_t num_queries, Dim dim,
        const float* centroids, uint32_t nlist,
        const std::vector<ClusterData>& clusters,
        const std::vector<std::vector<uint32_t>>& ground_truth,
        const rabitq::RotationMatrix& rotation);
};

}  // namespace index
}  // namespace vdb
