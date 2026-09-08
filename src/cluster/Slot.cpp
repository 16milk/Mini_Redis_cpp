#include "mini_redis/cluster/Slot.hpp"

#include <stdexcept>

namespace cluster {

HashTag findHashTag(std::string_view key) {
    const std::size_t open = key.find('{');
    if (open == std::string_view::npos) {
        return {};
    }
    const std::size_t close = key.find('}', open + 1);
    if (close == std::string_view::npos || close == open + 1) {
        return {};
    }
    HashTag tag;
    tag.present = true;
    tag.value = key.substr(open + 1, close - open - 1);
    return tag;
}

std::string_view slotKeyMaterial(std::string_view key) {
    const HashTag tag = findHashTag(key);
    return tag.present ? tag.value : key;
}

std::vector<SlotRange> splitSlotsEvenly(std::size_t shard_count) {
    if (shard_count == 0) {
        throw std::invalid_argument("shard count must be positive");
    }
    if (shard_count > static_cast<std::size_t>(kSlotCount)) {
        throw std::invalid_argument("shard count exceeds the 16384 slot space");
    }

    const std::size_t base = static_cast<std::size_t>(kSlotCount) / shard_count;
    const std::size_t remainder = static_cast<std::size_t>(kSlotCount) % shard_count;

    std::vector<SlotRange> ranges;
    ranges.reserve(shard_count);
    std::size_t next = 0;
    for (std::size_t index = 0; index < shard_count; ++index) {
        const std::size_t width = base + (index < remainder ? 1 : 0);
        SlotRange range;
        range.start = static_cast<SlotId>(next);
        range.end = static_cast<SlotId>(next + width - 1);
        ranges.push_back(range);
        next += width;
    }
    return ranges;
}

} // namespace cluster
