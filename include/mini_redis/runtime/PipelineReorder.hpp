#pragma once

#include "mini_redis/runtime/IoBudget.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace runtime {

// Connections issue monotonically increasing request_seq values. Different
// Raft groups may complete out of order; the reactor only writes the contiguous
// prefix starting at next_response_seq so a RESP pipeline stays in request order.
class PipelineReorder {
public:
    PipelineReorder() = default;
    explicit PipelineReorder(IoLimits limits) : limits_(std::move(limits)) {}

    void setLimits(IoLimits limits) { limits_ = std::move(limits); }
    const IoLimits& limits() const { return limits_; }

    bool canBegin() const { return inflight_ < limits_.max_inflight; }

    // Reserve a slot for a request that will later complete, possibly from
    // another thread. Returns false when the hard in-flight cap is reached.
    bool begin(std::uint64_t seq);

    void complete(std::uint64_t seq, std::string response);

    // Pop every consecutive ready response starting at next_response_seq.
    std::vector<std::string> takeReady();

    std::uint64_t nextResponseSeq() const { return next_response_seq_; }
    std::size_t inflight() const { return inflight_; }
    std::size_t buffered() const { return buffered_.size(); }
    std::size_t bufferedBytes() const { return buffered_bytes_; }

    bool pauseReads() const {
        return inflight_ >= limits_.inflight_high_watermark ||
               !canBegin();
    }
    bool belowResumeWatermark() const {
        return inflight_ <= limits_.inflight_low_watermark;
    }

    void reset();

private:
    IoLimits limits_;
    std::uint64_t next_response_seq_ = 1;
    std::size_t inflight_ = 0;
    std::size_t buffered_bytes_ = 0;
    std::map<std::uint64_t, std::string> buffered_;
};

}  // namespace runtime
