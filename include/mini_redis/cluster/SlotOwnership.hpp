#pragma once

#include "mini_redis/cluster/Topology.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace cluster {

enum class LocalSlotState : std::uint8_t {
    kUnowned = 0,
    kStable = 1,
    kSourceFenced = 2,
    kTargetActiveAsk = 3,
};

struct LocalSlotOwnership {
    ShardId shard = kNoShard;
    std::uint64_t epoch = 0;
    LocalSlotState state = LocalSlotState::kUnowned;
    std::string migration_id;
};

// This table belongs to a data Raft state machine, not to the routing cache.
// Its snapshot bytes are intended to be stored beside that group's data.
class SlotOwnershipTable {
public:
    const LocalSlotOwnership& get(SlotId slot) const { return slots_[slot]; }

    bool update(SlotId slot, std::uint64_t expected_epoch,
                LocalSlotOwnership replacement, std::string& error);
    bool canAcceptWrite(SlotId slot, ShardId shard, std::uint64_t epoch,
                        bool asking, std::string* error = nullptr) const;

    std::string snapshotBytes() const;
    bool installSnapshot(const std::string& bytes, std::string& error);

private:
    std::array<LocalSlotOwnership, kSlotCount> slots_{};
};

} // namespace cluster
