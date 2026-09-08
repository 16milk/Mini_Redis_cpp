#pragma once

#include "mini_redis/cluster/Slot.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cluster {

using NodeId = std::string;

// Identifies a logical shard, which is exactly one Raft group. Shard ids are
// 1-based so that 0 can mean "this slot has no owner".
using ShardId = std::uint32_t;
constexpr ShardId kNoShard = 0;

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;

    bool empty() const { return host.empty() || port == 0; }

    // Renders the address the way a cluster-aware client expects it inside a
    // MOVED/ASK payload. IPv6 literals are bracketed.
    std::string toRedirectTarget() const;
};

struct NodeRecord {
    NodeId id;
    Endpoint client;  // the client advertise address, not the internal Raft address
};

// One logical shard: a single Raft group whose voters replicate every slot the
// shard owns.
struct ShardRecord {
    ShardId id = kNoShard;
    std::vector<NodeId> voters;
};

// Published by a data group leader with its term and a short validity window.
// It is deliberately not part of the committed topology, so a leader election
// does not require a metadata Raft round.
struct LeaderHint {
    NodeId node_id;
    std::uint64_t term = 0;
    std::int64_t expires_at_ms = 0;
};

// A slot in the middle of moving from one shard (Raft group) to another.
struct SlotMigration {
    SlotId slot = 0;
    ShardId source = kNoShard;
    ShardId target = kNoShard;
    // True once the source group has committed TargetReady(proof), which is
    // what opens the ASK window. Before that the source still answers locally.
    bool target_ready = false;
};

// An immutable view of committed cluster metadata plus the currently trusted
// leader hints. Readers hold a shared_ptr, so a topology update never mutates a
// snapshot another request is already routing against.
class TopologySnapshot {
public:
    std::uint64_t configEpoch() const { return config_epoch_; }

    const std::vector<NodeRecord>& nodes() const { return nodes_; }
    const std::vector<ShardRecord>& shards() const { return shards_; }

    std::size_t nodeCount() const { return nodes_.size(); }

    // The number of logical shards, which is also the number of Raft groups,
    // Raft election timers and WAL streams on the cluster.
    std::size_t shardCount() const { return shards_.size(); }
    std::size_t raftGroupCount() const { return shards_.size(); }

    const NodeRecord* findNode(const NodeId& node_id) const;
    const ShardRecord* findShard(ShardId shard_id) const;

    ShardId slotOwner(SlotId slot) const { return slot_owner_[slot]; }
    std::size_t assignedSlotCount() const { return assigned_slot_count_; }
    bool allSlotsAssigned() const {
        return assigned_slot_count_ == static_cast<std::size_t>(kSlotCount);
    }

    const SlotMigration* migrationForSlot(SlotId slot) const;
    std::size_t migratingSlotCount() const { return migrations_.size(); }
    const LeaderHint* leaderHint(ShardId shard_id) const;

    // Returns the leader of `shard_id` only when the hint is still valid and the
    // hinted node is a current voter. An expired or unknown hint yields nullopt,
    // which callers must translate into TRYAGAIN rather than a guessed address.
    std::optional<NodeId> trustedLeader(ShardId shard_id, std::int64_t now_ms) const;

    bool nodeHostsShard(const NodeId& node_id, ShardId shard_id) const;
    std::vector<ShardId> shardsHostedBy(const NodeId& node_id) const;

    // Contiguous slot ranges owned by one shard, ascending.
    std::vector<SlotRange> ownedRanges(ShardId shard_id) const;

    // Every assigned slot range with its owning shard, ascending by start slot.
    // This is the source for CLUSTER SLOTS.
    std::vector<std::pair<SlotRange, ShardId>> slotRanges() const;

private:
    friend class TopologyBuilder;

    TopologySnapshot() { slot_owner_.fill(kNoShard); }

    std::uint64_t config_epoch_ = 0;
    std::vector<NodeRecord> nodes_;
    std::vector<ShardRecord> shards_;
    std::array<ShardId, kSlotCount> slot_owner_{};
    std::size_t assigned_slot_count_ = 0;
    std::unordered_map<SlotId, SlotMigration> migrations_;
    std::unordered_map<ShardId, LeaderHint> leader_hints_;
    std::unordered_map<NodeId, std::size_t> node_index_;
    std::unordered_map<ShardId, std::size_t> shard_index_;
};

using TopologyPtr = std::shared_ptr<const TopologySnapshot>;

// Builds and validates a snapshot. In the finished system the input comes from
// the committed metadata Raft group; here it comes from a config file or a test.
class TopologyBuilder {
public:
    TopologyBuilder& setConfigEpoch(std::uint64_t epoch);
    TopologyBuilder& addNode(NodeId node_id, std::string host, std::uint16_t port);
    TopologyBuilder& addShard(ShardId shard_id, std::vector<NodeId> voters);
    TopologyBuilder& assignSlots(ShardId shard_id, SlotRange range);
    TopologyBuilder& assignSlot(ShardId shard_id, SlotId slot);
    TopologyBuilder& setLeaderHint(ShardId shard_id, NodeId node_id, std::uint64_t term,
                                   std::int64_t expires_at_ms);
    TopologyBuilder& setMigration(SlotId slot, ShardId source, ShardId target,
                                  bool target_ready);

    // Throws std::invalid_argument when the description is not self-consistent.
    TopologyPtr build() const;

private:
    std::uint64_t config_epoch_ = 0;
    std::vector<NodeRecord> nodes_;
    std::vector<ShardRecord> shards_;
    std::vector<std::pair<ShardId, SlotRange>> slot_assignments_;
    std::vector<std::pair<ShardId, LeaderHint>> leader_hints_;
    std::vector<SlotMigration> migrations_;
};

} // namespace cluster
