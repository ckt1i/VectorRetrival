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
    // Reset per-query state
    ready_clusters_.clear();
    pending_.clear();
    next_to_submit_ = 0;
    inflight_clusters_ = 0;

    SearchContext ctx(query_vec, config_);
    RerankConsumer reranker(ctx, index_.dim());

    auto sorted_clusters = index_.FindNearestClusters(
        ctx.query_vec(), config_.nprobe);

    PrefetchClusters(ctx, sorted_clusters);
    ProbeAndDrainInterleaved(ctx, reranker, sorted_clusters);
    FinalDrain(ctx, reranker);

    auto results = ctx.collector().Finalize();

    FetchMissingPayloads(ctx, reranker, results);
    auto sr = AssembleResults(reranker, results);
    sr.stats() = ctx.stats();
    return sr;
}

// ============================================================================
// SubmitClusterRead + PrefetchClusters
// ============================================================================

void OverlapScheduler::SubmitClusterRead(uint32_t cluster_id) {
    auto loc = index_.segment().GetBlockLocation(cluster_id);
    if (!loc.has_value()) return;

    int clu_fd = index_.segment().clu_fd();
    uint32_t size32 = static_cast<uint32_t>(loc->size);
    // Not from BufferPool — ownership transfers to ParsedCluster.block_buf
    uint8_t* buf = new uint8_t[size32];
    reader_.PrepRead(clu_fd, buf, size32, loc->offset);

    PendingIO io;
    io.type = PendingIO::Type::CLUSTER_BLOCK;
    io.cluster_id = cluster_id;
    io.block_size = size32;
    pending_[buf] = io;
    inflight_clusters_++;
}

void OverlapScheduler::PrefetchClusters(
    SearchContext& ctx,
    const std::vector<ClusterID>& sorted_clusters) {
    uint32_t initial = std::min(
        config_.prefetch_depth,
        static_cast<uint32_t>(sorted_clusters.size()));
    for (uint32_t i = 0; i < initial; ++i) {
        SubmitClusterRead(sorted_clusters[i]);
    }
    next_to_submit_ = initial;

    if (initial > 0) {
        uint32_t submitted = reader_.Submit();
        ctx.stats().total_io_submitted += submitted;
    }
}

// ============================================================================
// DispatchCompletion
// ============================================================================

void OverlapScheduler::DispatchCompletion(
    uint8_t* buf, SearchContext& ctx, RerankConsumer& reranker) {
    auto it = pending_.find(buf);
    if (it == pending_.end()) return;

    PendingIO io = std::move(it->second);
    pending_.erase(it);

    switch (io.type) {
        case PendingIO::Type::CLUSTER_BLOCK: {
            inflight_clusters_--;
            auto block_buf = std::unique_ptr<uint8_t[]>(buf);
            ParsedCluster pc;
            auto s = index_.segment().ParseClusterBlock(
                io.cluster_id, std::move(block_buf), io.block_size, pc);
            if (s.ok()) {
                ready_clusters_[io.cluster_id] = std::move(pc);
            }
            // Do NOT Release to BufferPool — ownership moved to ParsedCluster
            break;
        }
        case PendingIO::Type::VEC_ONLY:
            reranker.ConsumeVec(buf, io.addr);
            buffer_pool_.Release(buf);
            break;
        case PendingIO::Type::VEC_ALL:
            reranker.ConsumeAll(buf, io.addr);
            buffer_pool_.Release(buf);
            break;
        case PendingIO::Type::PAYLOAD:
            reranker.ConsumePayload(buf, io.addr);
            // ConsumePayload takes ownership — don't release
            break;
    }
}

// ============================================================================
// ProbeCluster
// ============================================================================

void OverlapScheduler::ProbeCluster(
    const ParsedCluster& pc, uint32_t cluster_id,
    SearchContext& ctx, RerankConsumer& reranker) {

    const auto& conann = index_.conann();
    rabitq::RaBitQEstimator estimator(index_.dim());
    int dat_fd = index_.segment().data_reader().fd();

    ctx.stats().total_probed += pc.num_records;

    const float* centroid = index_.centroid(cluster_id);
    auto pq = estimator.PrepareQuery(
        ctx.query_vec(), centroid, index_.rotation());

    uint32_t batch_size = config_.probe_batch_size;
    std::vector<rabitq::RaBitQCode> batch_codes(batch_size);
    std::vector<float> dists(batch_size);

    for (uint32_t offset = 0; offset < pc.num_records; offset += batch_size) {
        uint32_t actual = std::min(batch_size, pc.num_records - offset);

        for (uint32_t i = 0; i < actual; ++i) {
            const uint64_t* words = reinterpret_cast<const uint64_t*>(
                pc.codes_start + (offset + i) * pc.code_entry_size);
            batch_codes[i].code.assign(words, words + num_words_);
            batch_codes[i].norm = 0.0f;
            batch_codes[i].sum_x = simd::PopcountTotal(words, num_words_);
        }

        estimator.EstimateDistanceBatch(
            pq, batch_codes.data(), actual, dists.data());

        for (uint32_t i = 0; i < actual; ++i) {
            ResultClass rc = conann.Classify(dists[i]);
            AddressEntry addr = pc.decoded_addresses[offset + i];

            if (rc == ResultClass::SafeOut) {
                ctx.stats().total_safe_out++;
                continue;
            }

            PendingIO pio;
            pio.addr = addr;

            if (rc == ResultClass::SafeIn) {
                ctx.stats().total_safe_in++;
                if (addr.size <= config_.safein_all_threshold) {
                    uint8_t* buf = buffer_pool_.Acquire(addr.size);
                    reader_.PrepRead(dat_fd, buf, addr.size, addr.offset);
                    pio.type = PendingIO::Type::VEC_ALL;
                    pio.read_offset = addr.offset;
                    pio.read_length = addr.size;
                    pending_[buf] = pio;
                } else {
                    uint8_t* buf = buffer_pool_.Acquire(vec_bytes_);
                    reader_.PrepRead(dat_fd, buf, vec_bytes_, addr.offset);
                    pio.type = PendingIO::Type::VEC_ONLY;
                    pio.read_offset = addr.offset;
                    pio.read_length = vec_bytes_;
                    pending_[buf] = pio;
                }
            } else {
                ctx.stats().total_uncertain++;
                uint8_t* buf = buffer_pool_.Acquire(vec_bytes_);
                reader_.PrepRead(dat_fd, buf, vec_bytes_, addr.offset);
                pio.type = PendingIO::Type::VEC_ONLY;
                pio.read_offset = addr.offset;
                pio.read_length = vec_bytes_;
                pending_[buf] = pio;
            }
        }
    }
}

// ============================================================================
// ProbeAndDrainInterleaved — unified event loop
// ============================================================================

void OverlapScheduler::ProbeAndDrainInterleaved(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<ClusterID>& sorted_clusters) {

    std::vector<IoCompletion> comps(128);

    for (size_t i = 0; i < sorted_clusters.size(); ++i) {
        uint32_t cid = sorted_clusters[i];

        // 1. Wait until target cluster is ready (consuming other CQEs meanwhile)
        while (ready_clusters_.find(cid) == ready_clusters_.end()) {
            uint32_t n = reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].buffer, ctx, reranker);
            }
        }

        // 2. Probe this cluster
        ProbeCluster(ready_clusters_[cid], cid, ctx, reranker);

        // 3. Release ParsedCluster memory
        ready_clusters_.erase(cid);

        // 4. Submit vec reads produced by this cluster's probe
        uint32_t submitted = reader_.Submit();
        ctx.stats().total_io_submitted += submitted;

        // Early stop: check if TopK quality already exceeds calibration
        // baseline d_k. TopK reflects completions consumed during
        // WaitAndPoll above (naturally interleaved with cluster waits).
        if (config_.early_stop &&
            ctx.collector().Full() &&
            ctx.collector().TopDistance() < index_.conann().d_k()) {
            ctx.stats().early_stopped = true;
            ctx.stats().clusters_skipped =
                static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
            break;
        }

        // 5. Sliding window refill
        if (inflight_clusters_ < config_.refill_threshold &&
            next_to_submit_ < sorted_clusters.size()) {
            uint32_t refill = std::min(
                config_.refill_count,
                static_cast<uint32_t>(sorted_clusters.size()) -
                    next_to_submit_);
            for (uint32_t r = 0; r < refill; ++r) {
                SubmitClusterRead(sorted_clusters[next_to_submit_++]);
            }
            if (refill > 0) {
                uint32_t sub = reader_.Submit();
                ctx.stats().total_io_submitted += sub;
            }
        }
    }
}

// ============================================================================
// FinalDrain
// ============================================================================

void OverlapScheduler::FinalDrain(SearchContext& ctx,
                                   RerankConsumer& reranker) {
    std::vector<IoCompletion> comps(128);
    while (!pending_.empty()) {
        uint32_t n = reader_.WaitAndPoll(
            comps.data(), static_cast<uint32_t>(comps.size()));
        for (uint32_t j = 0; j < n; ++j) {
            DispatchCompletion(comps[j].buffer, ctx, reranker);
        }
    }
}

// ============================================================================
// FetchMissingPayloads
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

        PendingIO pio;
        pio.type = PendingIO::Type::PAYLOAD;
        pio.addr = entry.addr;
        pio.read_offset = payload_off;
        pio.read_length = payload_len;
        pending_[buf] = pio;
    }

    if (!pending_.empty()) {
        uint32_t submitted = reader_.Submit();
        ctx.stats().total_io_submitted += submitted;
        ctx.stats().total_payload_fetched += submitted;

        std::vector<IoCompletion> comps(128);
        while (!pending_.empty()) {
            uint32_t n = reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].buffer, ctx, reranker);
            }
        }
    }
}

// ============================================================================
// AssembleResults
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
