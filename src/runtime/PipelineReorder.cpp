#include "mini_redis/runtime/PipelineReorder.hpp"

namespace runtime {

bool PipelineReorder::begin(std::uint64_t seq) {
    (void)seq;
    if (!canBegin()) {
        return false;
    }
    ++inflight_;
    return true;
}

void PipelineReorder::complete(std::uint64_t seq, std::string response) {
    if (seq < next_response_seq_) {
        return;
    }
    auto [it, inserted] = buffered_.emplace(seq, std::move(response));
    if (inserted) {
        buffered_bytes_ += it->second.size();
    }
}

std::vector<std::string> PipelineReorder::takeReady() {
    std::vector<std::string> ready;
    while (true) {
        auto it = buffered_.find(next_response_seq_);
        if (it == buffered_.end()) {
            break;
        }
        buffered_bytes_ -= it->second.size();
        ready.push_back(std::move(it->second));
        buffered_.erase(it);
        ++next_response_seq_;
        if (inflight_ > 0) {
            --inflight_;
        }
    }
    return ready;
}

void PipelineReorder::reset() {
    next_response_seq_ = 1;
    inflight_ = 0;
    buffered_bytes_ = 0;
    buffered_.clear();
}

}  // namespace runtime
