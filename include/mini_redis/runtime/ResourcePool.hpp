#pragma once

#include "mini_redis/runtime/IoBudget.hpp"

#include <array>
#include <cstddef>
#include <mutex>

namespace runtime {

// Per-class connection and buffer quotas. Admitting a snapshot connection
// cannot consume the Raft pool, so votes still have room when bulk traffic
// is saturated.
class ResourcePools {
public:
    ResourcePools();

    bool tryAdmit(TrafficClass cls);
    void release(TrafficClass cls);

    bool tryReserveBytes(TrafficClass cls, std::size_t bytes);
    void releaseBytes(TrafficClass cls, std::size_t bytes);

    std::size_t connections(TrafficClass cls) const;
    std::size_t bufferBytes(TrafficClass cls) const;
    std::size_t maxConnections(TrafficClass cls) const;
    bool isSaturated(TrafficClass cls) const;

    void setQuota(TrafficClass cls, ResourceQuota quota);

private:
    struct Slot {
        ResourceQuota quota;
        std::size_t connections = 0;
        std::size_t buffer_bytes = 0;
    };

    mutable std::mutex mutex_;
    std::array<Slot, kTrafficClassCount> slots_{};
};

}  // namespace runtime
