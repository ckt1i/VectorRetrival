#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "vdb/common/status.h"
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

    /// Pre-packed FastScan block (one per 32-vector group).
    /// When provided, ComputeScoresForQueryRaBitQ skips PackSignBitsForFastScan.
    struct FsBlock {
        const uint8_t* packed;  // PackSignBitsForFastScan output (FastScan layout)
        const float* norms;     // float32 norms[32]
        uint32_t count;         // actual vectors in this block (≤32)
    };
    const FsBlock* fs_blocks = nullptr;  // array of FsBlock; null → fallback path
    uint32_t num_fs_blocks = 0;          // 0 → use codes_block path
};

/// Per-query scores and predictions at each nprobe step.
/// This is the abstract data unit CRC calibration needs — distance-space agnostic.
struct QueryScores {
    std::vector<float> raw_scores;                  // [nlist] kth-dist at each step
    std::vector<std::vector<uint32_t>> predictions; // [nlist][≤K] top-K IDs at each step
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

    /// Evaluation results from CRC calibration.
    /// Metrics measure search-space self-consistency: whether early-stop
    /// misses vectors that full-probe would have found. NOT a comparison
    /// against exact L2 ground truth.
    struct EvalResults {
        float actual_fnr = 0.0f;     // measured FNR on test set
        float avg_probed = 0.0f;     // average clusters probed on test set
        float recall_at_1 = 0.0f;
        float recall_at_5 = 0.0f;
        float recall_at_10 = 0.0f;
        uint32_t test_size = 0;
    };

    /// Core calibration — distance-space agnostic.
    /// Ground truth is derived from predictions[nlist-1] (full-probe result).
    /// @param config      Calibration hyperparameters.
    /// @param all_scores  Pre-computed scores for all calibration queries.
    /// @param nlist       Number of clusters.
    /// @return Calibration parameters + test set evaluation results.
    static std::pair<CalibrationResults, EvalResults> Calibrate(
        const Config& config,
        const std::vector<QueryScores>& all_scores,
        uint32_t nlist);

    /// Convenience: calibrate from raw vectors using exact L2 distances.
    /// Computes QueryScores internally, then delegates to core Calibrate.
    static std::pair<CalibrationResults, EvalResults> Calibrate(
        const Config& config,
        const float* queries, uint32_t num_queries, Dim dim,
        const float* centroids, uint32_t nlist,
        const std::vector<ClusterData>& clusters);

    /// Convenience: calibrate using RaBitQ estimated distances.
    /// Computes QueryScores internally, then delegates to core Calibrate.
    /// Requires: ClusterData::codes_block and code_entry_size must be set.
    static std::pair<CalibrationResults, EvalResults> CalibrateWithRaBitQ(
        const Config& config,
        const float* queries, uint32_t num_queries, Dim dim,
        const float* centroids, uint32_t nlist,
        const std::vector<ClusterData>& clusters,
        const rabitq::RotationMatrix& rotation);

    // ----- Score computation (exposed for IvfBuilder) -----

    /// Compute QueryScores for all queries using RaBitQ estimated distances.
    /// This is the same probing logic used internally, exposed for offline
    /// score precomputation at build time.
    static std::vector<QueryScores> ComputeScoresRaBitQ(
        const float* queries, uint32_t num_queries, Dim dim,
        const float* centroids, uint32_t nlist,
        const std::vector<ClusterData>& clusters,
        uint32_t top_k,
        const rabitq::RotationMatrix& rotation);

    // ----- QueryScores serialization -----

    /// Write pre-computed scores to a binary file.
    static Status WriteScores(const std::string& path,
                              const std::vector<QueryScores>& scores,
                              uint32_t nlist, uint32_t top_k);

    /// Read pre-computed scores from a binary file.
    static Status ReadScores(const std::string& path,
                             std::vector<QueryScores>& scores,
                             uint32_t& nlist, uint32_t& top_k);
};

}  // namespace index
}  // namespace vdb
