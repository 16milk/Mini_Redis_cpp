#pragma once

#include "mini_redis/cluster/Crc16.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cluster {

using SlotId = std::uint16_t;

// Redis Cluster fixes the routing space at 16384 slots: the mask is a single
// AND and a full slot bitmap costs only 2 KiB per node.
constexpr int kSlotCount = 16384;
constexpr std::uint16_t kSlotMask = 0x3fff;

struct HashTag {
    bool present = false;
    std::string_view value;
};

// Redis hash-tag rule: take the bytes between the first '{' and the first '}'
// that follows it, and only when that span is non-empty. A missing '}' or an
// empty "{}" falls back to the whole key, and no later brace pair is examined.
HashTag findHashTag(std::string_view key);

// The byte range actually fed to CRC16 for this key.
std::string_view slotKeyMaterial(std::string_view key);

inline SlotId keyToSlot(std::string_view key) {
    return static_cast<SlotId>(crc16(slotKeyMaterial(key)) & kSlotMask);
}

struct SlotRange {
    SlotId start = 0;
    SlotId end = 0;  // inclusive, like CLUSTER SLOTS

    std::size_t count() const {
        return static_cast<std::size_t>(end) - static_cast<std::size_t>(start) + 1;
    }
    bool contains(SlotId slot) const { return slot >= start && slot <= end; }
};

// Splits the 16384 routing slots into `shard_count` contiguous ranges. Slots are
// only a routing unit: this is what keeps the number of Raft groups equal to the
// number of logical shards rather than to the number of slots.
std::vector<SlotRange> splitSlotsEvenly(std::size_t shard_count);

} // namespace cluster
