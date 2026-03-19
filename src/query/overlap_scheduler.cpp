#include "vdb/query/overlap_scheduler.h"

#include <cstring>

#include "vdb/common/distance.h"
#include "vdb/query/rerank_consumer.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/popcount.h"

namespace vdb {
namespace query {

OverlapScheduler::OverlapScheduler(index::IvfIndex& index,
                                   AsyncReader& reader,
                                   const SearchConfig& config)
    : index_(index),
      reader_(reader),
      config_(config),
      vec_bytes_(index.dim() * sizeof(float)),
      num_words_((index.dim() + 63) / 64) {}

OverlapScheduler::~OverlapScheduler() = default;

SearchResults OverlapScheduler::Search(const float* query_vec) {
    SearchContext ctx(query_vec, config_);
    RerankConsumer reranker(ctx, index_.dim());

    ProbeAndSubmit(ctx, reranker);
    DrainAndRerank(ctx, reranker);

    auto results = ctx.collector().Finalize();

    FetchMissingPayloads(ctx, reranker, results);
    return AssembleResults(reranker, results);
}

// ============================================================================
// Phase 1: ProbeAndSubmit
// ============================================================================

void OverlapScheduler::ProbeAndSubmit(SearchContext& ctx,
                                       RerankConsumer& reranker) {
    auto clusters = index_.FindNearestClusters(ctx.query_vec(), config_.nprobe);
    const auto& conann = index_.conann();

    rabitq::RaBitQEstimator estimator(index_.dim());
    int fd = index_.segment().data_reader().fd();

    for (auto cid : clusters) {
        auto s = index_.segment().EnsureClusterLoaded(cid);
        if (!s.ok()) continue;

        uint32_t n_records = index_.segment().GetNumRecords(cid);
        ctx.stats().total_probed += n_records;

        // Prepare RaBitQ query for this cluster
        const float* centroid = index_.centroid(cid);
        auto pq = estimator.PrepareQuery(ctx.query_vec(), centroid,
                                          index_.rotation());

        // Process in mini-batches
        uint32_t batch_size = config_.probe_batch_size;
        std::vector<rabitq::RaBitQCode> batch_codes(batch_size);
        std::vector<float> dists(batch_size);

        for (uint32_t offset = 0; offset < n_records; offset += batch_size) {
            uint32_t actual = std::min(batch_size, n_records - offset);

            // Build RaBitQCode batch from codes_buffer
            for (uint32_t i = 0; i < actual; ++i) {
                const uint64_t* words = reinterpret_cast<const uint64_t*>(
                    index_.segment().GetCodePtr(cid, offset + i));
                batch_codes[i].code.assign(words, words + num_words_);
                batch_codes[i].norm = 0.0f;  // Not persisted (known limitation)
                batch_codes[i].sum_x = simd::PopcountTotal(words, num_words_);
            }

            estimator.EstimateDistanceBatch(pq, batch_codes.data(), actual,
                                             dists.data());

            // Classify and schedule reads
            for (uint32_t i = 0; i < actual; ++i) {
                ResultClass rc = conann.Classify(dists[i]);
                AddressEntry addr = index_.segment().GetAddress(cid, offset + i);

                if (rc == ResultClass::SafeOut) {
                    ctx.stats().total_safe_out++;
                    continue;
                }

                if (rc == ResultClass::SafeIn) {
                    ctx.stats().total_safe_in++;
                    // SafeIn: read ALL if small enough, else VEC_ONLY
                    if (addr.size <= config_.safein_all_threshold) {
                        uint8_t* buf = buffer_pool_.Acquire(addr.size);
                        reader_.PrepRead(fd, buf, addr.size, addr.offset);
                        pending_[buf] = {addr, addr.offset, addr.size};
                    } else {
                        uint8_t* buf = buffer_pool_.Acquire(vec_bytes_);
                        reader_.PrepRead(fd, buf, vec_bytes_, addr.offset);
                        pending_[buf] = {addr, addr.offset, vec_bytes_};
                    }
                } else {
                    // Uncertain: read VEC_ONLY
                    ctx.stats().total_uncertain++;
                    uint8_t* buf = buffer_pool_.Acquire(vec_bytes_);
                    reader_.PrepRead(fd, buf, vec_bytes_, addr.offset);
                    pending_[buf] = {addr, addr.offset, vec_bytes_};
                }
            }
        }

        // Flush after each cluster
        uint32_t submitted = reader_.Submit();
        ctx.stats().total_io_submitted += submitted;
    }
}

// ============================================================================
// Phase 2: DrainAndRerank
// ============================================================================

ReadTaskType OverlapScheduler::InferType(const PendingRead& pr) const {
    if (pr.read_length == vec_bytes_) {
        return ReadTaskType::VEC_ONLY;
    }
    if (pr.read_offset == pr.addr.offset && pr.read_length == pr.addr.size) {
        return ReadTaskType::ALL;
    }
    return ReadTaskType::PAYLOAD;
}

void OverlapScheduler::DrainAndRerank(SearchContext& ctx,
                                       RerankConsumer& reranker) {
    std::vector<IoCompletion> comps(128);

    while (!pending_.empty()) {
        uint32_t n = reader_.WaitAndPoll(comps.data(),
                                          static_cast<uint32_t>(comps.size()));
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t* buf = comps[i].buffer;
            auto it = pending_.find(buf);
            if (it == pending_.end()) continue;

            PendingRead pr = it->second;
            pending_.erase(it);

            ReadTaskType type = InferType(pr);

            switch (type) {
                case ReadTaskType::VEC_ONLY:
                    reranker.ConsumeVec(buf, pr.addr);
                    buffer_pool_.Release(buf);
                    break;
                case ReadTaskType::ALL:
                    reranker.ConsumeAll(buf, pr.addr);
                    buffer_pool_.Release(buf);
                    break;
                case ReadTaskType::PAYLOAD:
                    reranker.ConsumePayload(buf, pr.addr);
                    // ConsumePayload takes ownership — don't release
                    break;
            }
        }
    }
}

// ============================================================================
// Phase 3: FetchMissingPayloads
// ============================================================================

void OverlapScheduler::FetchMissingPayloads(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    int fd = index_.segment().data_reader().fd();

    for (const auto& entry : results) {
        if (reranker.HasPayload(entry.addr.offset)) continue;
        if (entry.addr.size <= vec_bytes_) continue;  // No payload data

        uint32_t payload_len = entry.addr.size - vec_bytes_;
        uint64_t payload_off = entry.addr.offset + vec_bytes_;
        uint8_t* buf = buffer_pool_.Acquire(payload_len);
        reader_.PrepRead(fd, buf, payload_len, payload_off);
        pending_[buf] = {entry.addr, payload_off, payload_len};
    }

    if (!pending_.empty()) {
        uint32_t submitted = reader_.Submit();
        ctx.stats().total_io_submitted += submitted;
        ctx.stats().total_payload_fetched += submitted;

        // Drain all payload reads
        std::vector<IoCompletion> comps(128);
        while (!pending_.empty()) {
            uint32_t n = reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t* buf = comps[i].buffer;
                auto it = pending_.find(buf);
                if (it == pending_.end()) continue;
                PendingRead pr = it->second;
                pending_.erase(it);
                reranker.ConsumePayload(buf, pr.addr);
            }
        }
    }
}

// ============================================================================
// Phase 4: AssembleResults
// ============================================================================

SearchResults OverlapScheduler::AssembleResults(
    RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    SearchResults sr;
    sr.results().reserve(results.size());

    const auto& schemas = index_.segment().data_reader().payload_schemas();
    bool has_payload = !schemas.empty();

    for (const auto& entry : results) {
        SearchResult result;
        result.distance = entry.distance;

        if (has_payload) {
            auto payload_buf = reranker.TakePayload(entry.addr.offset);
            if (payload_buf && entry.addr.size > vec_bytes_) {
                uint32_t payload_len = entry.addr.size - vec_bytes_;
                index_.segment().data_reader().ParsePayload(
                    payload_buf.get(), payload_len, 0, result.payload);
            }
        }

        sr.results().push_back(std::move(result));
    }

    return sr;
}

}  // namespace query
}  // namespace vdb
