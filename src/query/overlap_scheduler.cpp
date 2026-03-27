#include "vdb/query/overlap_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>

#include "vdb/common/distance.h"
#include "vdb/query/rerank_consumer.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace query {

OverlapScheduler::OverlapScheduler(index::IvfIndex& index,
                                   AsyncReader& reader,
                                   const SearchConfig& config)
    : index_(index),
      reader_(reader),
      config_(config),
      vec_bytes_(index.dim() * sizeof(float)),
      num_words_((index.dim() + 63) / 64),
      est_top_k_(config.top_k),
      use_crc_(config.crc_params != nullptr) {
    if (use_crc_) {
        crc_stopper_ = index::CrcStopper(*config.crc_params, index.nlist());
        est_heap_.reserve(est_top_k_);
    }
}

OverlapScheduler::~OverlapScheduler() = default;

SearchResults OverlapScheduler::Search(const float* query_vec) {
    // Reset per-query state
    ready_clusters_.clear();
    pending_.clear();
    next_to_submit_ = 0;
    inflight_clusters_ = 0;
    est_heap_.clear();

    auto t_search_start = std::chrono::steady_clock::now();

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
    sr.stats().total_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_search_start).count();
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
        use_crc_ ? config_.initial_prefetch : config_.prefetch_depth,
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

    rabitq::RaBitQEstimator estimator(index_.dim());
    int dat_fd = index_.segment().data_reader().fd();

    ctx.stats().total_probed += pc.num_records;

    const float* centroid = index_.centroid(cluster_id);
    auto pq = estimator.PrepareQuery(
        ctx.query_vec(), centroid, index_.rotation());

    // Dynamic margin: margin = 2 · r_max · r_q · ε_ip
    float r_max = pc.epsilon;  // .clu lookup field stores r_max
    float eps_ip = index_.conann().epsilon();
    float margin = 2.0f * r_max * pq.norm_qc * eps_ip;

    // CRC: compute dynamic_d_k from est_heap_ at cluster start (not updated
    // within this cluster — only between clusters for stability).
    float dynamic_d_k = (use_crc_ && est_heap_.size() >= est_top_k_)
        ? est_heap_.front().first
        : index_.conann().d_k();

    const uint32_t batch_size = config_.probe_batch_size;
    std::vector<float> dists(batch_size);
    const uint32_t norm_byte_offset = num_words_ * sizeof(uint64_t);

    for (uint32_t offset = 0; offset < pc.num_records; offset += batch_size) {
        uint32_t actual = std::min(batch_size, pc.num_records - offset);

        // Zero-copy: read code/norm directly from block_buf, no heap allocation
        for (uint32_t i = 0; i < actual; ++i) {
            const auto* entry =
                pc.codes_start + (offset + i) * pc.code_entry_size;
            const auto* words = reinterpret_cast<const uint64_t*>(entry);
            float norm_oc;
            std::memcpy(&norm_oc, entry + norm_byte_offset, sizeof(float));
            dists[i] = estimator.EstimateDistanceRaw(
                pq, words, num_words_, norm_oc);
        }

        // Update est_heap_ with ALL RaBitQ estimates (before classify, so
        // even SafeOut vectors contribute to a tighter dynamic_d_k for the
        // next cluster).
        if (use_crc_) {
            for (uint32_t i = 0; i < actual; ++i) {
                if (est_heap_.size() < est_top_k_) {
                    est_heap_.push_back({dists[i], offset + i});
                    std::push_heap(est_heap_.begin(), est_heap_.end());
                } else if (dists[i] < est_heap_.front().first) {
                    std::pop_heap(est_heap_.begin(), est_heap_.end());
                    est_heap_.back() = {dists[i], offset + i};
                    std::push_heap(est_heap_.begin(), est_heap_.end());
                }
            }
        }

        for (uint32_t i = 0; i < actual; ++i) {
            ResultClass rc = use_crc_
                ? index_.conann().ClassifyAdaptive(dists[i], margin, dynamic_d_k)
                : index_.conann().Classify(dists[i], margin);
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
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].buffer, ctx, reranker);
            }
        }

        // 2. Probe this cluster
        {
            auto tp0 = std::chrono::steady_clock::now();
            ProbeCluster(ready_clusters_[cid], cid, ctx, reranker);
            ctx.stats().probe_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tp0).count();
        }

        // 3. Release ParsedCluster memory
        ready_clusters_.erase(cid);

        // 4. Submit vec reads produced by this cluster's probe
        uint32_t submitted = reader_.Submit();
        ctx.stats().total_io_submitted += submitted;

        // Early stop
        if (config_.early_stop) {
            if (use_crc_) {
                // CRC early stop: use RaBitQ estimate heap distance
                float est_kth = (est_heap_.size() >= est_top_k_)
                    ? est_heap_.front().first
                    : std::numeric_limits<float>::infinity();
                if (crc_stopper_.ShouldStop(
                        static_cast<uint32_t>(i + 1), est_kth)) {
                    ctx.stats().early_stopped = true;
                    ctx.stats().clusters_skipped =
                        static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                    ctx.stats().crc_clusters_probed =
                        static_cast<uint32_t>(i + 1);
                    break;
                }
            } else {
                // Legacy early stop: exact L2 collector vs static d_k
                if (ctx.collector().Full() &&
                    ctx.collector().TopDistance() < index_.conann().d_k()) {
                    ctx.stats().early_stopped = true;
                    ctx.stats().clusters_skipped =
                        static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                    break;
                }
            }
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
    // Phase 1: Drain all in-flight async IOs (io_uring path).
    while (reader_.InFlight() > 0) {
        auto tw0 = std::chrono::steady_clock::now();
        uint32_t n = reader_.WaitAndPoll(
            comps.data(), static_cast<uint32_t>(comps.size()));
        ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tw0).count();
        for (uint32_t j = 0; j < n; ++j) {
            DispatchCompletion(comps[j].buffer, ctx, reranker);
        }
    }
    // Phase 2: Drain any already-completed reads (sync pread path).
    // PreadFallbackReader completes reads in Submit() so InFlight() == 0,
    // but completions still sit in its internal deque.
    for (;;) {
        uint32_t n = reader_.Poll(
            comps.data(), static_cast<uint32_t>(comps.size()));
        if (n == 0) break;
        for (uint32_t j = 0; j < n; ++j) {
            DispatchCompletion(comps[j].buffer, ctx, reranker);
        }
    }
    // Phase 3: Free any orphaned pending buffers (stale completions from
    // a previous query whose buffers no longer match pending_ entries).
    for (auto& [buf, pio] : pending_) {
        if (pio.type == PendingIO::Type::CLUSTER_BLOCK) {
            delete[] buf;
        } else {
            buffer_pool_.Release(buf);
        }
    }
    pending_.clear();
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
        while (reader_.InFlight() > 0) {
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].buffer, ctx, reranker);
            }
        }
        // Drain sync completions (pread path)
        for (;;) {
            uint32_t n = reader_.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
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
