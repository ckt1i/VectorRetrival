#include "vdb/query/overlap_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "vdb/query/rerank_consumer.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace query {

namespace {
inline void CheckPrepRead(const Status& s, const char* context) {
    if (!s.ok()) {
        std::fprintf(stderr, "FATAL: PrepRead failed (%s): %s\n",
                     context, s.ToString().c_str());
        std::abort();
    }
}

inline const char* CluReadModeName(CluReadMode mode) {
    switch (mode) {
        case CluReadMode::Window:
            return "window";
        case CluReadMode::FullPreload:
            return "full_preload";
    }
    return "unknown";
}
}  // namespace

OverlapScheduler::OverlapScheduler(index::IvfIndex& index,
                                   AsyncReader& reader,
                                   const SearchConfig& config)
    : OverlapScheduler(index, reader, reader, config) {}

OverlapScheduler::OverlapScheduler(index::IvfIndex& index,
                                   AsyncReader& cluster_reader,
                                   AsyncReader& data_reader,
                                   const SearchConfig& config)
    : index_(index),
      cluster_reader_(cluster_reader),
      data_reader_(data_reader),
      isolated_submission_mode_(&cluster_reader != &data_reader ||
                                config.submission_mode == SubmissionMode::Isolated),
      config_(config),
      vec_bytes_(index.dim() * sizeof(float)),
      est_top_k_(config.top_k),
      use_crc_(config.crc_params != nullptr),
      estimator_(index.dim(), index.segment().rabitq_config().bits),
      prober_(index.conann(), index.dim(),
              index.segment().rabitq_config().bits) {
    if (index.used_hadamard()) {
        rotated_q_.resize(index.dim());
    }
    if (use_crc_) {
        crc_stopper_ = index::CrcStopper(*config.crc_params, index.nlist());
        est_heap_.reserve(est_top_k_);
    }
    // Stage 2: precompute margin divisor from bits
    uint8_t bits = index.segment().rabitq_config().bits;
    if (bits > 1) {
        has_s2_ = true;
        margin_s2_divisor_ = static_cast<float>(1u << (bits - 1));
    }
    buffer_pool_.Prime(vec_bytes_, std::max(1u, config_.io_queue_depth));
    InitializeDataBufferSlab();
}

OverlapScheduler::~OverlapScheduler() {
    CleanupPendingSlots();
    for (uint8_t* buf : fixed_vec_buffers_) {
        std::free(buf);
    }
}

SearchResults OverlapScheduler::Search(const float* query_vec) {
    // Reset per-query state
    ready_clusters_.clear();
    resident_query_clusters_.clear();
    CleanupPendingSlots();
    next_to_submit_ = 0;
    inflight_clusters_ = 0;
    est_heap_.clear();

    auto t_search_start = std::chrono::steady_clock::now();

    if (config_.clu_read_mode == CluReadMode::FullPreload &&
        !index_.segment().resident_preload_enabled()) {
        Status s = index_.segment().PreloadAllClusters();
        if (!s.ok()) {
            std::fprintf(stderr,
                         "FATAL: failed to enable %s mode: %s\n",
                         CluReadModeName(config_.clu_read_mode),
                         s.ToString().c_str());
            std::abort();
        }
    }

    // Phase 1.3: precompute P^T × query once when Hadamard rotation is used
    if (index_.used_hadamard()) {
        index_.rotation().Apply(query_vec, rotated_q_.data());
    }

    SearchContext ctx(query_vec, config_);
    RerankConsumer reranker(ctx, index_.dim());

    auto sorted_clusters = index_.FindNearestClusters(
        ctx.query_vec(), config_.nprobe);

    PrefetchClusters(ctx, sorted_clusters);
    ProbeAndDrainInterleaved(ctx, reranker, sorted_clusters);
    FinalDrain(ctx, reranker);

    auto results = ctx.collector().Finalize();

    {
        auto tf0 = std::chrono::steady_clock::now();
        FetchMissingPayloads(ctx, reranker, results);
        ctx.stats().fetch_missing_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tf0).count();
    }
    auto sr = AssembleResults(reranker, results);
    sr.stats() = ctx.stats();
    sr.stats().total_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_search_start).count();
    return sr;
}

uint32_t OverlapScheduler::AllocatePendingSlot(PendingIO io, uint8_t* buffer,
                                               PendingBufferCleanup cleanup,
                                               uint16_t fixed_buffer_index) {
    uint32_t slot_id = 0;
    if (!free_pending_slots_.empty()) {
        slot_id = free_pending_slots_.back();
        free_pending_slots_.pop_back();
    } else {
        slot_id = static_cast<uint32_t>(pending_slots_.size());
        pending_slots_.emplace_back();
    }

    PendingSlot& slot = pending_slots_[slot_id];
    slot.in_use = true;
    slot.buffer = buffer;
    slot.fixed_buffer_index = fixed_buffer_index;
    slot.cleanup = cleanup;
    slot.io = std::move(io);
    return slot_id;
}

OverlapScheduler::PendingSlot* OverlapScheduler::GetPendingSlot(
    uint64_t slot_token) {
    uint32_t slot_id = static_cast<uint32_t>(slot_token);
    if (slot_token > std::numeric_limits<uint32_t>::max() ||
        slot_id >= pending_slots_.size()) {
        return nullptr;
    }
    PendingSlot& slot = pending_slots_[slot_id];
    return slot.in_use ? &slot : nullptr;
}

void OverlapScheduler::ReleasePendingSlot(uint32_t slot_id) {
    if (slot_id >= pending_slots_.size()) return;
    PendingSlot& slot = pending_slots_[slot_id];
    slot.in_use = false;
    slot.buffer = nullptr;
    slot.fixed_buffer_index = 0;
    slot.cleanup = PendingBufferCleanup::None;
    slot.io = PendingIO{};
    free_pending_slots_.push_back(slot_id);
}

void OverlapScheduler::ReleaseFixedVecBuffer(uint16_t buffer_index) {
    free_fixed_vec_buffers_.push_back(buffer_index);
}

void OverlapScheduler::CleanupPendingSlot(PendingSlot& slot) {
    if (!slot.in_use || !slot.buffer) return;

    switch (slot.cleanup) {
        case PendingBufferCleanup::Free:
            std::free(slot.buffer);
            break;
        case PendingBufferCleanup::Pool:
            buffer_pool_.Release(slot.buffer);
            break;
        case PendingBufferCleanup::FixedVec:
            ReleaseFixedVecBuffer(slot.fixed_buffer_index);
            break;
        case PendingBufferCleanup::None:
            break;
    }
    slot.buffer = nullptr;
    slot.cleanup = PendingBufferCleanup::None;
}

void OverlapScheduler::CleanupPendingSlots() {
    for (uint32_t slot_id = 0; slot_id < pending_slots_.size(); ++slot_id) {
        PendingSlot& slot = pending_slots_[slot_id];
        if (!slot.in_use) continue;
        CleanupPendingSlot(slot);
        ReleasePendingSlot(slot_id);
    }
}

bool OverlapScheduler::TryAcquireFixedVecBuffer(uint8_t** buffer,
                                                uint16_t* buffer_index) {
    if (!fixed_vec_buffers_enabled_ || free_fixed_vec_buffers_.empty()) {
        return false;
    }
    uint16_t idx = free_fixed_vec_buffers_.back();
    free_fixed_vec_buffers_.pop_back();
    *buffer = fixed_vec_buffers_[idx];
    *buffer_index = idx;
    return true;
}

void OverlapScheduler::InitializeDataBufferSlab() {
    auto* uring_reader = dynamic_cast<IoUringReader*>(&data_reader_);
    if (uring_reader == nullptr || config_.io_queue_depth == 0) {
        return;
    }

    fixed_vec_buffers_.reserve(config_.io_queue_depth);
    fixed_vec_buffer_capacities_.reserve(config_.io_queue_depth);
    free_fixed_vec_buffers_.reserve(config_.io_queue_depth);
    const uint32_t aligned_vec_bytes = (vec_bytes_ + 4095u) & ~4095u;
    for (uint32_t i = 0; i < config_.io_queue_depth; ++i) {
        uint8_t* buf = static_cast<uint8_t*>(
            std::aligned_alloc(4096, aligned_vec_bytes));
        if (!buf) {
            for (uint8_t* allocated : fixed_vec_buffers_) {
                std::free(allocated);
            }
            fixed_vec_buffers_.clear();
            fixed_vec_buffer_capacities_.clear();
            free_fixed_vec_buffers_.clear();
            return;
        }
        fixed_vec_buffers_.push_back(buf);
        fixed_vec_buffer_capacities_.push_back(aligned_vec_bytes);
        free_fixed_vec_buffers_.push_back(
            static_cast<uint16_t>(fixed_vec_buffers_.size() - 1));
    }

    std::vector<const uint8_t*> raw_ptrs(fixed_vec_buffers_.size());
    for (size_t i = 0; i < fixed_vec_buffers_.size(); ++i) {
        raw_ptrs[i] = fixed_vec_buffers_[i];
    }
    Status s = uring_reader->RegisterBuffers(raw_ptrs.data(),
                                             fixed_vec_buffer_capacities_.data(),
                                             static_cast<uint32_t>(raw_ptrs.size()));
    if (!s.ok()) {
        for (uint8_t* buf : fixed_vec_buffers_) {
            std::free(buf);
        }
        fixed_vec_buffers_.clear();
        fixed_vec_buffer_capacities_.clear();
        free_fixed_vec_buffers_.clear();
        return;
    }

    fixed_buffer_reader_ = uring_reader;
    fixed_vec_buffers_enabled_ = true;
}

// ============================================================================
// SubmitClusterRead + PrefetchClusters
// ============================================================================

void OverlapScheduler::SubmitClusterRead(uint32_t cluster_id) {
    auto loc = index_.segment().GetBlockLocation(cluster_id);
    if (!loc.has_value()) return;

    int clu_fd = index_.segment().clu_fd();
    uint32_t size32 = static_cast<uint32_t>(loc->size);
    // Round up read length to 4KB for O_DIRECT compatibility
    uint32_t read_size = (size32 + 4095u) & ~4095u;
    // 4KB-aligned buffer allocation for O_DIRECT compatibility
    uint8_t* buf = static_cast<uint8_t*>(
        std::aligned_alloc(4096, read_size));
    if (!buf) {
        std::fprintf(stderr, "FATAL: aligned_alloc failed for cluster block (%u bytes)\n", read_size);
        std::abort();
    }
    PendingIO io;
    io.type = PendingIO::Type::CLUSTER_BLOCK;
    io.cluster_id = cluster_id;
    io.block_size = size32;  // original size for parsing
    uint32_t slot_id = AllocatePendingSlot(std::move(io), buf,
                                           PendingBufferCleanup::Free);
    CheckPrepRead(cluster_reader_.PrepReadTagged(
                      clu_fd, buf, read_size, loc->offset, slot_id),
                  "cluster block");
    inflight_clusters_++;
}

void OverlapScheduler::PrefetchClusters(
    SearchContext& ctx,
    const std::vector<ClusterID>& sorted_clusters) {
    if (config_.clu_read_mode == CluReadMode::FullPreload) {
        next_to_submit_ = static_cast<uint32_t>(sorted_clusters.size());
        inflight_clusters_ = 0;
        (void)ctx;
        return;
    }
    uint32_t initial = std::min(
        use_crc_ ? config_.initial_prefetch : config_.prefetch_depth,
        static_cast<uint32_t>(sorted_clusters.size()));
    for (uint32_t i = 0; i < initial; ++i) {
        SubmitClusterRead(sorted_clusters[i]);
    }
    next_to_submit_ = initial;

    if (initial > 0) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = cluster_reader_.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }
    }
}

const ParsedCluster* OverlapScheduler::GetResidentParsedCluster(
    uint32_t cluster_id) {
    auto it = resident_query_clusters_.find(cluster_id);
    if (it != resident_query_clusters_.end()) {
        return &it->second;
    }
    const auto* resident = index_.segment().GetResidentClusterView(cluster_id);
    if (resident == nullptr) {
        return nullptr;
    }
    auto [inserted_it, _] = resident_query_clusters_.emplace(
        cluster_id, resident->ToParsedCluster());
    return &inserted_it->second;
}

// ============================================================================
// DispatchCompletion
// ============================================================================

void OverlapScheduler::DispatchCompletion(
    uint64_t slot_token, SearchContext& ctx, RerankConsumer& reranker) {
    PendingSlot* slot = GetPendingSlot(slot_token);
    if (slot == nullptr) return;

    const uint32_t slot_id = static_cast<uint32_t>(slot_token);
    uint8_t* buf = slot->buffer;
    PendingIO io = slot->io;

    switch (io.type) {
        case PendingIO::Type::CLUSTER_BLOCK: {
            inflight_clusters_--;
            auto block_buf = AlignedBufPtr(buf);
            slot->buffer = nullptr;
            slot->cleanup = PendingBufferCleanup::None;
            ParsedCluster pc;
            {
                auto tp0 = std::chrono::steady_clock::now();
                auto s = index_.segment().ParseClusterBlock(
                    io.cluster_id, std::move(block_buf), io.block_size, pc);
                ctx.stats().parse_cluster_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tp0).count();
                if (s.ok()) {
                    ready_clusters_[io.cluster_id] = std::move(pc);
                }
            }
            // Do NOT Release to BufferPool — ownership moved to ParsedCluster
            break;
        }
        case PendingIO::Type::VEC_ONLY: {
            auto t_rerank_start = std::chrono::steady_clock::now();
            reranker.ConsumeVec(buf, io.addr);
            ctx.stats().rerank_cpu_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_rerank_start).count();
            CleanupPendingSlot(*slot);
            break;
        }
        case PendingIO::Type::VEC_ALL: {
            auto t_rerank_start = std::chrono::steady_clock::now();
            reranker.ConsumeAll(buf, io.addr);
            ctx.stats().rerank_cpu_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_rerank_start).count();
            CleanupPendingSlot(*slot);
            break;
        }
        case PendingIO::Type::PAYLOAD:
            reranker.ConsumePayload(buf, io.addr);
            slot->buffer = nullptr;
            slot->cleanup = PendingBufferCleanup::None;
            // ConsumePayload takes ownership — don't release
            break;
    }
    ReleasePendingSlot(slot_id);
}

// ============================================================================
// AsyncIOSink — ProbeResultSink that submits io_uring reads + tracks est_heap
// ============================================================================

class OverlapScheduler::AsyncIOSink : public index::ProbeResultSink {
 public:
    AsyncIOSink(OverlapScheduler& sched, int dat_fd)
        : sched_(sched), dat_fd_(dat_fd) {}

    void OnCandidate(uint32_t /*vec_idx*/,
                     AddressEntry addr,
                     float est_dist,
                     index::CandidateClass cls) override {
        // Maintain est_heap_ for dynamic SafeOut threshold across clusters
        if (sched_.use_crc_) {
            if (sched_.est_heap_.size() < sched_.est_top_k_) {
                sched_.est_heap_.push_back({est_dist, 0u});
                std::push_heap(sched_.est_heap_.begin(), sched_.est_heap_.end());
            } else if (est_dist < sched_.est_heap_.front().first) {
                std::pop_heap(sched_.est_heap_.begin(), sched_.est_heap_.end());
                sched_.est_heap_.back() = {est_dist, 0u};
                std::push_heap(sched_.est_heap_.begin(), sched_.est_heap_.end());
            }
        }

        PendingIO pio;
        pio.addr = addr;

        if (cls == index::CandidateClass::SafeIn) {
            if (addr.size <= sched_.config_.safein_all_threshold) {
                uint8_t* buf = sched_.buffer_pool_.Acquire(addr.size);
                pio.type = PendingIO::Type::VEC_ALL;
                pio.read_offset = addr.offset;
                pio.read_length = addr.size;
                uint32_t slot_id = sched_.AllocatePendingSlot(
                    pio, buf, PendingBufferCleanup::Pool);
                auto tp0 = std::chrono::steady_clock::now();
                CheckPrepRead(sched_.data_reader_.PrepReadTagged(
                                  dat_fd_, buf, addr.size, addr.offset, slot_id),
                              "SafeIn VEC_ALL");
                prep_read_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tp0).count();
            } else {
                uint8_t* buf = nullptr;
                uint16_t fixed_idx = 0;
                bool use_fixed_buffer =
                    sched_.TryAcquireFixedVecBuffer(&buf, &fixed_idx);
                if (!use_fixed_buffer) {
                    buf = sched_.buffer_pool_.Acquire(sched_.vec_bytes_);
                }
                pio.type = PendingIO::Type::VEC_ONLY;
                pio.read_offset = addr.offset;
                pio.read_length = sched_.vec_bytes_;
                uint32_t slot_id = sched_.AllocatePendingSlot(
                    pio, buf,
                    use_fixed_buffer ? PendingBufferCleanup::FixedVec
                                     : PendingBufferCleanup::Pool,
                    fixed_idx);
                auto tp0 = std::chrono::steady_clock::now();
                if (use_fixed_buffer && sched_.fixed_buffer_reader_ != nullptr) {
                    CheckPrepRead(sched_.fixed_buffer_reader_->PrepReadRegisteredBufferTagged(
                                      dat_fd_, buf, fixed_idx,
                                      sched_.vec_bytes_, addr.offset, slot_id),
                                  "SafeIn VEC_ONLY fixed");
                } else {
                    CheckPrepRead(sched_.data_reader_.PrepReadTagged(
                                      dat_fd_, buf, sched_.vec_bytes_,
                                      addr.offset, slot_id),
                                  "SafeIn VEC_ONLY");
                }
                prep_read_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tp0).count();
            }
        } else {  // Uncertain
            uint8_t* buf = nullptr;
            uint16_t fixed_idx = 0;
            bool use_fixed_buffer =
                sched_.TryAcquireFixedVecBuffer(&buf, &fixed_idx);
            if (!use_fixed_buffer) {
                buf = sched_.buffer_pool_.Acquire(sched_.vec_bytes_);
            }
            pio.type = PendingIO::Type::VEC_ONLY;
            pio.read_offset = addr.offset;
            pio.read_length = sched_.vec_bytes_;
            uint32_t slot_id = sched_.AllocatePendingSlot(
                pio, buf,
                use_fixed_buffer ? PendingBufferCleanup::FixedVec
                                 : PendingBufferCleanup::Pool,
                fixed_idx);
            auto tp0 = std::chrono::steady_clock::now();
            if (use_fixed_buffer && sched_.fixed_buffer_reader_ != nullptr) {
                CheckPrepRead(sched_.fixed_buffer_reader_->PrepReadRegisteredBufferTagged(
                                  dat_fd_, buf, fixed_idx,
                                  sched_.vec_bytes_, addr.offset, slot_id),
                              "Uncertain VEC_ONLY fixed");
            } else {
                CheckPrepRead(sched_.data_reader_.PrepReadTagged(
                                  dat_fd_, buf, sched_.vec_bytes_,
                                  addr.offset, slot_id),
                              "Uncertain VEC_ONLY");
            }
            prep_read_ms_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tp0).count();
        }
    }

    double prep_read_ms_ = 0;  // accumulated PrepRead() time, read by ProbeCluster()

 private:
    OverlapScheduler& sched_;
    int dat_fd_;
};

// ============================================================================
// ProbeCluster — Phase 3: thin wrapper around ClusterProber + AsyncIOSink
// ============================================================================

void OverlapScheduler::ProbeCluster(
    const ParsedCluster& pc, uint32_t cluster_id,
    SearchContext& ctx, RerankConsumer& /*reranker*/) {

    int dat_fd = index_.segment().data_reader().fd();
    ctx.stats().total_probed += pc.num_records;

    // Phase 1.3: fast path avoids per-cluster FWHT using precomputed rotated_q_
    // and precomputed rotated_centroid (P^T × c_k). Falls back to full rotation otherwise.
    if (index_.used_hadamard()) {
        estimator_.PrepareQueryRotatedInto(
            rotated_q_.data(), index_.rotated_centroid(cluster_id), &pq_);
    } else {
        estimator_.PrepareQueryInto(
            ctx.query_vec(), index_.centroid(cluster_id), index_.rotation(), &pq_);
    }

    float margin_factor = 2.0f * pq_.norm_qc * index_.conann().epsilon();

    // dynamic_d_k: infinity when est_heap not yet full (avoids false SafeOut
    // on early clusters before a reliable k-th estimate is available).
    float dynamic_d_k = (use_crc_ && est_heap_.size() >= est_top_k_)
        ? est_heap_.front().first
        : std::numeric_limits<float>::infinity();

    AsyncIOSink sink(*this, dat_fd);
    index::ProbeStats local_stats;
    prober_.Probe(pc, pq_, margin_factor, dynamic_d_k, sink, local_stats);
    ctx.stats().uring_prep_ms += sink.prep_read_ms_;

    // Aggregate classification statistics into SearchContext.
    // total_safe_in  = S1 SafeIn (I/O submitted)
    // total_safe_out = S1 SafeOut (skipped)
    // total_uncertain = final Uncertain candidates (S1 uncertain minus S2 eliminations)
    ctx.stats().total_safe_in   += local_stats.s1_safein;
    ctx.stats().total_safe_out  += local_stats.s1_safeout;
    ctx.stats().s2_safe_in      += local_stats.s2_safein;
    ctx.stats().s2_safe_out     += local_stats.s2_safeout;
    ctx.stats().s2_uncertain    += local_stats.s2_uncertain;
    ctx.stats().total_uncertain +=
        local_stats.s1_uncertain - local_stats.s2_safein - local_stats.s2_safeout;
}

// ============================================================================
// ProbeAndDrainInterleaved — unified event loop
// ============================================================================

void OverlapScheduler::ProbeAndDrainInterleaved(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<ClusterID>& sorted_clusters) {

    std::vector<IoCompletion> comps(128);
    const uint32_t reserve_slots = std::min(
        config_.cluster_submit_reserve,
        config_.io_queue_depth > 0 ? config_.io_queue_depth - 1 : 0);
    uint32_t vec_flush_threshold =
        (config_.submit_batch_size == 0) ? 1u : config_.submit_batch_size;
    if (config_.io_queue_depth > 0) {
        const uint32_t capacity_for_vec =
            std::max(1u, config_.io_queue_depth - reserve_slots);
        vec_flush_threshold = std::min(vec_flush_threshold, capacity_for_vec);
    }
    auto record_submit = [&](AsyncReader& reader) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = reader.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }
    };

    auto drain_completions = [&](AsyncReader& reader, bool wait_for_one) {
        uint32_t n = 0;
        auto tw0 = std::chrono::steady_clock::now();
        if (wait_for_one) {
            n = reader.WaitAndPoll(comps.data(),
                                   static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
        } else {
            n = reader.Poll(comps.data(), static_cast<uint32_t>(comps.size()));
        }
        for (uint32_t j = 0; j < n; ++j) {
            DispatchCompletion(comps[j].user_data, ctx, reranker);
        }
    };

    auto flush_shared_reader = [&](bool force, bool prioritize_cluster_reads) {
        if (cluster_reader_.prepped() == 0) return;
        const bool vec_batch_ready = cluster_reader_.prepped() >= vec_flush_threshold;
        if (!force && !prioritize_cluster_reads && !vec_batch_ready) return;
        record_submit(cluster_reader_);
    };

    auto flush_isolated_readers = [&](bool force,
                                      bool prioritize_cluster_reads,
                                      bool vec_batch_ready) {
        if (cluster_reader_.prepped() > 0 &&
            (force || prioritize_cluster_reads)) {
            record_submit(cluster_reader_);
        }
        if (data_reader_.prepped() > 0 &&
            (force || vec_batch_ready)) {
            record_submit(data_reader_);
        }
    };

    for (size_t i = 0; i < sorted_clusters.size(); ++i) {
        uint32_t cid = sorted_clusters[i];

        const ParsedCluster* cluster_to_probe = nullptr;
        if (config_.clu_read_mode == CluReadMode::FullPreload) {
            cluster_to_probe = GetResidentParsedCluster(cid);
            if (cluster_to_probe == nullptr) {
                std::fprintf(stderr,
                             "FATAL: resident cluster view missing for cluster %u\n",
                             cid);
                std::abort();
            }
        } else {
            // 1. Wait until target cluster is ready (consuming other CQEs meanwhile).
            while (ready_clusters_.find(cid) == ready_clusters_.end()) {
                if (isolated_submission_mode_) {
                    flush_isolated_readers(/*force=*/true,
                                           /*prioritize_cluster_reads=*/false,
                                           /*vec_batch_ready=*/true);
                    if (cluster_reader_.InFlight() > 0) {
                        drain_completions(cluster_reader_, /*wait_for_one=*/true);
                    } else {
                        drain_completions(cluster_reader_, /*wait_for_one=*/false);
                    }
                    drain_completions(data_reader_, /*wait_for_one=*/false);
                } else {
                    flush_shared_reader(/*force=*/true,
                                        /*prioritize_cluster_reads=*/false);
                    drain_completions(cluster_reader_, /*wait_for_one=*/true);
                }
            }
            cluster_to_probe = &ready_clusters_[cid];
        }

        {
            auto tp0 = std::chrono::steady_clock::now();
            ProbeCluster(*cluster_to_probe, cid, ctx, reranker);
            ctx.stats().probe_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tp0).count();
        }

        if (config_.clu_read_mode != CluReadMode::FullPreload) {
            ready_clusters_.erase(cid);
        }

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
                    if (isolated_submission_mode_) {
                        flush_isolated_readers(/*force=*/true,
                                               /*prioritize_cluster_reads=*/false,
                                               /*vec_batch_ready=*/true);
                    } else {
                        flush_shared_reader(/*force=*/true,
                                            /*prioritize_cluster_reads=*/false);
                    }
                    break;
                }
            } else {
                // Legacy early stop: exact L2 collector vs static d_k
                if (ctx.collector().Full() &&
                    ctx.collector().TopDistance() < index_.conann().d_k()) {
                    ctx.stats().early_stopped = true;
                    ctx.stats().clusters_skipped =
                        static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                    if (isolated_submission_mode_) {
                        flush_isolated_readers(/*force=*/true,
                                               /*prioritize_cluster_reads=*/false,
                                               /*vec_batch_ready=*/true);
                    } else {
                        flush_shared_reader(/*force=*/true,
                                            /*prioritize_cluster_reads=*/false);
                    }
                    break;
                }
            }
        }

        // 4. Sliding window refill — cluster reads are prepared first, then
        // coalesced with vec reads into a single submit when possible.
        bool prepared_cluster_reads = false;
        if (config_.clu_read_mode != CluReadMode::FullPreload) {
            if (inflight_clusters_ < config_.refill_threshold &&
                next_to_submit_ < sorted_clusters.size()) {
                uint32_t refill = std::min(
                    config_.refill_count,
                    static_cast<uint32_t>(sorted_clusters.size()) -
                        next_to_submit_);
                if (reserve_slots > 0) {
                    refill = std::min(refill, reserve_slots);
                }
                for (uint32_t r = 0; r < refill; ++r) {
                    SubmitClusterRead(sorted_clusters[next_to_submit_++]);
                }
                prepared_cluster_reads = refill > 0;
            }
        }

        if (isolated_submission_mode_) {
            const bool vec_batch_ready = data_reader_.prepped() >= vec_flush_threshold;
            flush_isolated_readers(/*force=*/false,
                                   /*prioritize_cluster_reads=*/prepared_cluster_reads,
                                   vec_batch_ready);
        } else {
            flush_shared_reader(/*force=*/false,
                                /*prioritize_cluster_reads=*/prepared_cluster_reads);
        }
    }

    if (isolated_submission_mode_) {
        flush_isolated_readers(/*force=*/true,
                               /*prioritize_cluster_reads=*/false,
                               /*vec_batch_ready=*/true);
    } else {
        flush_shared_reader(/*force=*/true,
                            /*prioritize_cluster_reads=*/false);
    }
}

// ============================================================================
// FinalDrain
// ============================================================================

void OverlapScheduler::FinalDrain(SearchContext& ctx,
                                   RerankConsumer& reranker) {
    std::vector<IoCompletion> comps(128);
    auto drain_reader = [&](AsyncReader& reader) {
        while (reader.InFlight() > 0) {
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = reader.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].user_data, ctx, reranker);
            }
        }
        for (;;) {
            uint32_t n = reader.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].user_data, ctx, reranker);
            }
        }
    };

    drain_reader(cluster_reader_);
    if (isolated_submission_mode_) {
        drain_reader(data_reader_);
    }
    CleanupPendingSlots();
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
        PendingIO pio;
        pio.type = PendingIO::Type::PAYLOAD;
        pio.addr = entry.addr;
        pio.read_offset = payload_off;
        pio.read_length = payload_len;
        uint32_t slot_id = AllocatePendingSlot(std::move(pio), buf,
                                               PendingBufferCleanup::Pool);
        CheckPrepRead(data_reader_.PrepReadTagged(
                          fd, buf, payload_len, payload_off, slot_id),
                      "payload");
    }

    if (data_reader_.prepped() > 0 || data_reader_.InFlight() > 0) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = data_reader_.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        ctx.stats().total_payload_fetched += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }

        std::vector<IoCompletion> comps(128);
        while (data_reader_.InFlight() > 0) {
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = data_reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].user_data, ctx, reranker);
            }
        }
        // Drain sync completions (pread path)
        for (;;) {
            uint32_t n = data_reader_.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].user_data, ctx, reranker);
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
