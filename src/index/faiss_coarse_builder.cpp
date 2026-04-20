#include "vdb/index/faiss_coarse_builder.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/MetricType.h>

namespace vdb {
namespace index {

namespace {

void NormalizeRows(std::vector<float>* matrix, uint32_t rows, uint32_t dim) {
    for (uint32_t i = 0; i < rows; ++i) {
        float* row = matrix->data() + static_cast<size_t>(i) * dim;
        float norm_sq = 0.0f;
        for (uint32_t j = 0; j < dim; ++j) {
            norm_sq += row[j] * row[j];
        }
        const float norm = std::sqrt(norm_sq);
        if (norm <= 0.0f) {
            continue;
        }
        for (uint32_t j = 0; j < dim; ++j) {
            row[j] /= norm;
        }
    }
}

}  // namespace

Status RunFaissCoarseBuilder(const float* vectors,
                             uint32_t num_vectors,
                             const FaissCoarseBuildConfig& config,
                             FaissCoarseBuildResult* result) {
    if (result == nullptr) {
        return Status::InvalidArgument("Faiss coarse builder result is null");
    }
    if (vectors == nullptr || num_vectors == 0 || config.dim == 0 || config.nlist == 0) {
        return Status::InvalidArgument("Faiss coarse builder received invalid input");
    }
    if (config.nlist > num_vectors) {
        return Status::InvalidArgument("Faiss coarse builder requires nlist <= num_vectors");
    }

    result->centroids.clear();
    result->assignments.clear();
    result->requested_metric = config.metric;

    faiss::MetricType faiss_metric = faiss::METRIC_L2;
    const bool normalize_for_cosine = (config.metric == "cosine");
    if (normalize_for_cosine || config.metric == "ip") {
        faiss_metric = faiss::METRIC_INNER_PRODUCT;
        result->effective_metric = "ip";
    } else {
        result->effective_metric = "l2";
    }

    std::vector<float> working(
        vectors, vectors + static_cast<size_t>(num_vectors) * config.dim);
    if (normalize_for_cosine) {
        NormalizeRows(&working, num_vectors, config.dim);
    }

    std::unique_ptr<faiss::Index> quantizer;
    if (faiss_metric == faiss::METRIC_INNER_PRODUCT) {
        quantizer = std::make_unique<faiss::IndexFlatIP>(config.dim);
    } else {
        quantizer = std::make_unique<faiss::IndexFlatL2>(config.dim);
    }

    faiss::IndexIVFFlat index_ivf(
        quantizer.get(), config.dim, config.nlist, faiss_metric);
    index_ivf.cp.niter = static_cast<int>(config.niter);
    index_ivf.cp.nredo = static_cast<int>(config.nredo);
    index_ivf.cp.seed = static_cast<int>(config.seed);
    index_ivf.cp.verbose = false;
    index_ivf.cp.spherical = (faiss_metric == faiss::METRIC_INNER_PRODUCT);
    index_ivf.quantizer_trains_alone = 0;

    const uint32_t effective_train_size = std::min(config.train_size, num_vectors);
    try {
        index_ivf.train(effective_train_size, working.data());
    } catch (const faiss::FaissException& e) {
        return Status::Internal(std::string("Faiss IVF coarse training failed: ") + e.what());
    } catch (const std::exception& e) {
        return Status::Internal(std::string("Faiss IVF coarse training failed: ") + e.what());
    }

    result->centroids.resize(static_cast<size_t>(config.nlist) * config.dim);
    try {
        index_ivf.quantizer->reconstruct_n(
            0, config.nlist, result->centroids.data());
    } catch (const faiss::FaissException& e) {
        return Status::Internal(std::string("Faiss quantizer centroid export failed: ") + e.what());
    } catch (const std::exception& e) {
        return Status::Internal(std::string("Faiss quantizer centroid export failed: ") + e.what());
    }

    std::vector<faiss::idx_t> labels(num_vectors);
    std::vector<float> distances(num_vectors);
    try {
        index_ivf.quantizer->search(
            num_vectors, working.data(), 1, distances.data(), labels.data());
    } catch (const faiss::FaissException& e) {
        return Status::Internal(std::string("Faiss quantizer assignment failed: ") + e.what());
    } catch (const std::exception& e) {
        return Status::Internal(std::string("Faiss quantizer assignment failed: ") + e.what());
    }

    result->assignments.resize(num_vectors);
    for (uint32_t i = 0; i < num_vectors; ++i) {
        if (labels[i] < 0 || labels[i] >= static_cast<faiss::idx_t>(config.nlist)) {
            return Status::Internal("Faiss quantizer assignment produced out-of-range label");
        }
        result->assignments[i] = static_cast<uint32_t>(labels[i]);
    }

    return Status::OK();
}

}  // namespace index
}  // namespace vdb
