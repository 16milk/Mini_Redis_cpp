#include "mini_redis/cluster/Topology.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cluster {
namespace {

bool looksLikeIpv6(const std::string& host) {
    return host.find(':') != std::string::npos;
}

} // namespace

std::string Endpoint::toRedirectTarget() const {
    if (looksLikeIpv6(host)) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

const NodeRecord* TopologySnapshot::findNode(const NodeId& node_id) const {
    const auto it = node_index_.find(node_id);
    return it == node_index_.end() ? nullptr : &nodes_[it->second];
}

const ShardRecord* TopologySnapshot::findShard(ShardId shard_id) const {
    const auto it = shard_index_.find(shard_id);
    return it == shard_index_.end() ? nullptr : &shards_[it->second];
}

const SlotMigration* TopologySnapshot::migrationForSlot(SlotId slot) const {
    const auto it = migrations_.find(slot);
    return it == migrations_.end() ? nullptr : &it->second;
}

const LeaderHint* TopologySnapshot::leaderHint(ShardId shard_id) const {
    const auto it = leader_hints_.find(shard_id);
    return it == leader_hints_.end() ? nullptr : &it->second;
}

std::optional<NodeId> TopologySnapshot::trustedLeader(ShardId shard_id,
                                                      std::int64_t now_ms) const {
    const LeaderHint* hint = leaderHint(shard_id);
    if (hint == nullptr || hint->node_id.empty()) {
        return std::nullopt;
    }
    if (hint->expires_at_ms <= now_ms) {
        return std::nullopt;  // stale hint: the caller must answer TRYAGAIN
    }
    if (!nodeHostsShard(hint->node_id, shard_id)) {
        return std::nullopt;  // the hinted node is no longer a voter of the group
    }
    if (findNode(hint->node_id) == nullptr) {
        return std::nullopt;  // no committed node record, so no advertise address
    }
    return hint->node_id;
}

bool TopologySnapshot::nodeHostsShard(const NodeId& node_id, ShardId shard_id) const {
    const ShardRecord* shard = findShard(shard_id);
    if (shard == nullptr) {
        return false;
    }
    return std::find(shard->voters.begin(), shard->voters.end(), node_id) !=
           shard->voters.end();
}

std::vector<ShardId> TopologySnapshot::shardsHostedBy(const NodeId& node_id) const {
    std::vector<ShardId> hosted;
    for (const ShardRecord& shard : shards_) {
        if (std::find(shard.voters.begin(), shard.voters.end(), node_id) !=
            shard.voters.end()) {
            hosted.push_back(shard.id);
        }
    }
    return hosted;
}

std::vector<SlotRange> TopologySnapshot::ownedRanges(ShardId shard_id) const {
    std::vector<SlotRange> ranges;
    int slot = 0;
    while (slot < kSlotCount) {
        if (slot_owner_[static_cast<std::size_t>(slot)] != shard_id) {
            ++slot;
            continue;
        }
        const int start = slot;
        while (slot < kSlotCount &&
               slot_owner_[static_cast<std::size_t>(slot)] == shard_id) {
            ++slot;
        }
        SlotRange range;
        range.start = static_cast<SlotId>(start);
        range.end = static_cast<SlotId>(slot - 1);
        ranges.push_back(range);
    }
    return ranges;
}

std::vector<std::pair<SlotRange, ShardId>> TopologySnapshot::slotRanges() const {
    std::vector<std::pair<SlotRange, ShardId>> ranges;
    int slot = 0;
    while (slot < kSlotCount) {
        const ShardId owner = slot_owner_[static_cast<std::size_t>(slot)];
        if (owner == kNoShard) {
            ++slot;
            continue;
        }
        const int start = slot;
        while (slot < kSlotCount &&
               slot_owner_[static_cast<std::size_t>(slot)] == owner) {
            ++slot;
        }
        SlotRange range;
        range.start = static_cast<SlotId>(start);
        range.end = static_cast<SlotId>(slot - 1);
        ranges.emplace_back(range, owner);
    }
    return ranges;
}

TopologyBuilder& TopologyBuilder::setConfigEpoch(std::uint64_t epoch) {
    topology_revision_ = epoch;
    return *this;
}

TopologyBuilder& TopologyBuilder::setTopologyRevision(std::uint64_t revision) {
    topology_revision_ = revision;
    return *this;
}

TopologyBuilder& TopologyBuilder::addNode(NodeId node_id, std::string host,
                                          std::uint16_t port) {
    NodeRecord record;
    record.id = std::move(node_id);
    record.client.host = std::move(host);
    record.client.port = port;
    nodes_.push_back(std::move(record));
    return *this;
}

TopologyBuilder& TopologyBuilder::addShard(ShardId shard_id, std::vector<NodeId> voters) {
    ShardRecord record;
    record.id = shard_id;
    record.voters = std::move(voters);
    shards_.push_back(std::move(record));
    return *this;
}

TopologyBuilder& TopologyBuilder::assignSlots(ShardId shard_id, SlotRange range) {
    slot_assignments_.emplace_back(shard_id, range);
    return *this;
}

TopologyBuilder& TopologyBuilder::assignSlot(ShardId shard_id, SlotId slot) {
    SlotRange range;
    range.start = slot;
    range.end = slot;
    return assignSlots(shard_id, range);
}

TopologyBuilder& TopologyBuilder::setSlotEpoch(SlotId slot, std::uint64_t epoch) {
    slot_epochs_.emplace_back(slot, epoch);
    return *this;
}

TopologyBuilder& TopologyBuilder::setLeaderHint(ShardId shard_id, NodeId node_id,
                                                std::uint64_t term,
                                                std::int64_t expires_at_ms) {
    LeaderHint hint;
    hint.node_id = std::move(node_id);
    hint.term = term;
    hint.expires_at_ms = expires_at_ms;
    leader_hints_.emplace_back(shard_id, std::move(hint));
    return *this;
}

TopologyBuilder& TopologyBuilder::setMigration(SlotId slot, ShardId source, ShardId target,
                                               bool target_ready,
                                               std::uint64_t from_epoch,
                                               std::uint64_t to_epoch) {
    SlotMigration migration;
    migration.slot = slot;
    migration.source = source;
    migration.target = target;
    migration.from_epoch = from_epoch;
    migration.to_epoch = to_epoch;
    migration.target_ready = target_ready;
    migrations_.push_back(migration);
    return *this;
}

TopologyPtr TopologyBuilder::build() const {
    // The snapshot is immutable once published, so every consistency rule is
    // enforced here instead of being re-checked on the request path.
    std::shared_ptr<TopologySnapshot> snapshot(new TopologySnapshot());
    snapshot->topology_revision_ = topology_revision_;
    // Ownership generations are independent from topology revisions. Static
    // bootstrap descriptions without explicit per-slot epochs start at 1.
    snapshot->slot_epoch_.fill(1);

    for (const NodeRecord& node : nodes_) {
        if (node.id.empty()) {
            throw std::invalid_argument("node id must not be empty");
        }
        if (node.client.empty()) {
            throw std::invalid_argument("node " + node.id +
                                        " has no client advertise address");
        }
        if (!snapshot->node_index_.emplace(node.id, snapshot->nodes_.size()).second) {
            throw std::invalid_argument("duplicate node id " + node.id);
        }
        snapshot->nodes_.push_back(node);
    }

    for (const ShardRecord& shard : shards_) {
        if (shard.id == kNoShard) {
            throw std::invalid_argument("shard id 0 is reserved for 'unassigned'");
        }
        if (shard.voters.empty()) {
            throw std::invalid_argument("shard " + std::to_string(shard.id) +
                                        " has no Raft voters");
        }
        for (const NodeId& voter : shard.voters) {
            if (snapshot->node_index_.find(voter) == snapshot->node_index_.end()) {
                throw std::invalid_argument("shard " + std::to_string(shard.id) +
                                            " references unknown node " + voter);
            }
        }
        if (!snapshot->shard_index_.emplace(shard.id, snapshot->shards_.size()).second) {
            throw std::invalid_argument("duplicate shard id " + std::to_string(shard.id));
        }
        snapshot->shards_.push_back(shard);
    }

    for (const auto& [shard_id, range] : slot_assignments_) {
        if (snapshot->shard_index_.find(shard_id) == snapshot->shard_index_.end()) {
            throw std::invalid_argument("slot range assigned to unknown shard " +
                                        std::to_string(shard_id));
        }
        if (range.start > range.end || range.end >= kSlotCount) {
            throw std::invalid_argument("invalid slot range " +
                                        std::to_string(range.start) + "-" +
                                        std::to_string(range.end));
        }
        for (int slot = range.start; slot <= range.end; ++slot) {
            ShardId& owner = snapshot->slot_owner_[static_cast<std::size_t>(slot)];
            if (owner != kNoShard && owner != shard_id) {
                throw std::invalid_argument("slot " + std::to_string(slot) +
                                            " is claimed by two shards");
            }
            if (owner == kNoShard) {
                owner = shard_id;
                ++snapshot->assigned_slot_count_;
            }
        }
    }

    for (const auto& [slot, epoch] : slot_epochs_) {
        if (slot >= kSlotCount || snapshot->slot_owner_[slot] == kNoShard) {
            throw std::invalid_argument("slot epoch references an unassigned slot");
        }
        if (epoch == 0) {
            throw std::invalid_argument("slot epoch must be non-zero");
        }
        snapshot->slot_epoch_[slot] = epoch;
    }

    for (const auto& [shard_id, hint] : leader_hints_) {
        if (!snapshot->nodeHostsShard(hint.node_id, shard_id)) {
            throw std::invalid_argument("leader hint for shard " +
                                        std::to_string(shard_id) + " names " +
                                        hint.node_id + ", which is not a voter");
        }
        snapshot->leader_hints_[shard_id] = hint;
    }

    for (SlotMigration migration : migrations_) {
        if (migration.slot >= kSlotCount) {
            throw std::invalid_argument("migration slot out of range");
        }
        if (snapshot->shard_index_.find(migration.target) ==
            snapshot->shard_index_.end()) {
            throw std::invalid_argument("migration targets unknown shard " +
                                        std::to_string(migration.target));
        }
        const ShardId owner = snapshot->slot_owner_[migration.slot];
        if (owner == kNoShard) {
            throw std::invalid_argument("migration of unassigned slot " +
                                        std::to_string(migration.slot));
        }
        if (migration.source != owner) {
            // Ownership only moves once the target group is committed as owner,
            // so during the window the source must still be the committed owner.
            throw std::invalid_argument("migration source for slot " +
                                        std::to_string(migration.slot) +
                                        " is not the committed owner");
        }
        if (migration.source == migration.target) {
            throw std::invalid_argument("migration source and target are identical");
        }
        const std::uint64_t owner_epoch = snapshot->slot_epoch_[migration.slot];
        if (migration.from_epoch == 0) migration.from_epoch = owner_epoch;
        if (migration.to_epoch == 0 &&
            migration.from_epoch != std::numeric_limits<std::uint64_t>::max()) {
            migration.to_epoch = migration.from_epoch + 1;
        }
        if (migration.from_epoch != owner_epoch ||
            migration.to_epoch <= migration.from_epoch) {
            throw std::invalid_argument("migration ownership epochs are invalid");
        }
        snapshot->migrations_[migration.slot] = migration;
    }

    return snapshot;
}

} // namespace cluster
