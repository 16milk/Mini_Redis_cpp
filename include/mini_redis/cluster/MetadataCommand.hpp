#pragma once

#include "mini_redis/cluster/Metadata.hpp"

#include <cstdint>
#include <string>

namespace cluster {

constexpr std::uint16_t kMetadataCommandVersion = 1;

enum class MetadataOperation : std::uint8_t {
    kInitializeCluster = 1,
    kUpsertNode = 2,
    kRemoveNode = 3,
    kUpsertGroup = 4,
    kRemoveGroup = 5,
    kAssignSlots = 6,
    kBeginMigration = 7,
    kAdvanceMigration = 8,
    kCommitMigration = 9,
    kFinishMigration = 10,
};

// A single stable wire shape keeps Raft payload decoding simple. Only fields
// relevant to `operation` are interpreted by the state machine.
struct MetadataCommand {
    std::uint16_t version = kMetadataCommandVersion;
    MetadataOperation operation = MetadataOperation::kInitializeCluster;
    std::string request_id;
    std::uint64_t expected_revision = 0;

    std::string cluster_id;
    MetadataNodeRecord node;
    NodeId node_id;
    MetadataGroupRecord group;
    ShardId group_id = kNoShard;

    SlotId slot_start = 0;
    SlotId slot_end = 0;
    ShardId owner_group = kNoShard;
    std::uint64_t expected_ownership_epoch = 0;
    std::uint64_t new_ownership_epoch = 0;

    std::string migration_id;
    ShardId source_group = kNoShard;
    ShardId target_group = kNoShard;
    MigrationPhase migration_phase = MigrationPhase::kPreparing;
    std::string progress_proof;
};

std::string encodeMetadataCommand(const MetadataCommand& command);
bool decodeMetadataCommand(const std::string& encoded, MetadataCommand& command,
                           std::string& error);

} // namespace cluster
