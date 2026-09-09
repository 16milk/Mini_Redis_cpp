#pragma once

#include "mini_redis/cluster/Topology.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cluster {

enum class MetadataNodeStatus : std::uint8_t {
    kJoining = 1,
    kActive = 2,
    kDraining = 3,
    kRemoved = 4,
};

struct MetadataNodeRecord {
    NodeId id;
    Endpoint client;
    Endpoint internal;
    Endpoint admin;
    std::string failure_domain;
    MetadataNodeStatus status = MetadataNodeStatus::kActive;
};

struct MetadataGroupRecord {
    ShardId id = kNoShard;
    std::vector<NodeId> voters;
    std::vector<NodeId> learners;
    std::vector<NodeId> desired_placement;
    std::uint64_t config_index = 0;
};

enum class SlotTransition : std::uint8_t {
    kStable = 1,
    kMigrating = 2,
};

struct MetadataSlotRecord {
    ShardId active_group = kNoShard;
    std::uint64_t ownership_epoch = 0;
    SlotTransition transition = SlotTransition::kStable;
    std::string migration_id;
};

enum class MigrationPhase : std::uint8_t {
    kPreparing = 1,
    kPrepared = 2,
    kCopyingBase = 3,
    kCatchingUp = 4,
    kSourceFenced = 5,
    kTargetActiveAsk = 6,
    kMetadataCommitted = 7,
    kCleanup = 8,
    kAborting = 9,
    kAborted = 10,
};

struct MetadataMigrationRecord {
    std::string id;
    SlotId slot = 0;
    ShardId source = kNoShard;
    ShardId target = kNoShard;
    std::uint64_t from_epoch = 0;
    std::uint64_t to_epoch = 0;
    MigrationPhase phase = MigrationPhase::kPreparing;
    std::string progress_proof;
};

// Logical state applied by the dedicated metadata Raft group. Ordered maps make
// snapshots deterministic across replicas.
struct ClusterMetadata {
    std::string cluster_id;
    std::uint64_t revision = 0;
    std::map<NodeId, MetadataNodeRecord> nodes;
    std::map<ShardId, MetadataGroupRecord> groups;
    std::array<MetadataSlotRecord, kSlotCount> slots{};
    std::map<std::string, MetadataMigrationRecord> migrations;

    bool initialized() const { return !cluster_id.empty(); }
};

} // namespace cluster
