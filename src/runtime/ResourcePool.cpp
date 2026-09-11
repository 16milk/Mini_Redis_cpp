#include "mini_redis/runtime/ResourcePool.hpp"

namespace runtime {

ResourcePools::ResourcePools() {
    for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
        slots_[index].quota = quotaFor(static_cast<TrafficClass>(index));
    }
}

bool ResourcePools::tryAdmit(TrafficClass cls) {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (slot.connections >= slot.quota.max_connections) {
        return false;
    }
    ++slot.connections;
    return true;
}

void ResourcePools::release(TrafficClass cls) {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (slot.connections > 0) {
        --slot.connections;
    }
}

bool ResourcePools::tryReserveBytes(TrafficClass cls, std::size_t bytes) {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (slot.buffer_bytes > slot.quota.max_buffer_bytes ||
        bytes > slot.quota.max_buffer_bytes - slot.buffer_bytes) {
        return false;
    }
    slot.buffer_bytes += bytes;
    return true;
}

void ResourcePools::releaseBytes(TrafficClass cls, std::size_t bytes) {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (bytes >= slot.buffer_bytes) {
        slot.buffer_bytes = 0;
    } else {
        slot.buffer_bytes -= bytes;
    }
}

std::size_t ResourcePools::connections(TrafficClass cls) const {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_[index].connections;
}

std::size_t ResourcePools::bufferBytes(TrafficClass cls) const {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_[index].buffer_bytes;
}

std::size_t ResourcePools::maxConnections(TrafficClass cls) const {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_[index].quota.max_connections;
}

bool ResourcePools::isSaturated(TrafficClass cls) const {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    const Slot& slot = slots_[index];
    return slot.connections >= slot.quota.max_connections ||
           slot.buffer_bytes >= slot.quota.max_buffer_bytes;
}

void ResourcePools::setQuota(TrafficClass cls, ResourceQuota quota) {
    const auto index = static_cast<std::size_t>(cls);
    std::lock_guard<std::mutex> lock(mutex_);
    slots_[index].quota = quota;
}

}  // namespace runtime
