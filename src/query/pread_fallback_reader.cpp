#include "vdb/query/async_reader.h"

#include <unistd.h>
#include <cerrno>
#include <algorithm>

namespace vdb {
namespace query {

Status PreadFallbackReader::PrepReadTagged(int fd, uint8_t* buf,
                                           uint32_t len, uint64_t offset,
                                           uint64_t user_data) {
    pending_.push_back({fd, buf, len, offset, user_data});
    return Status::OK();
}

uint32_t PreadFallbackReader::Submit() {
    uint32_t count = 0;
    for (auto& entry : pending_) {
        ssize_t ret = ::pread(entry.fd, entry.buf, entry.len,
                              static_cast<off_t>(entry.offset));
        IoCompletion comp;
        comp.buffer = entry.buf;
        comp.user_data = entry.user_data;
        comp.result = (ret >= 0) ? static_cast<int32_t>(ret)
                                 : -static_cast<int32_t>(errno);
        completed_.push_back(comp);
        ++count;
    }
    pending_.clear();
    return count;
}

uint32_t PreadFallbackReader::Poll(IoCompletion* out, uint32_t max_count) {
    uint32_t n = std::min(max_count, static_cast<uint32_t>(completed_.size()));
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = completed_.front();
        completed_.pop_front();
    }
    return n;
}

uint32_t PreadFallbackReader::WaitAndPoll(IoCompletion* out,
                                           uint32_t max_count) {
    // Synchronous: all completions are already available after Submit
    return Poll(out, max_count);
}

uint32_t PreadFallbackReader::InFlight() const {
    return 0;  // All reads complete synchronously in Submit
}

}  // namespace query
}  // namespace vdb
