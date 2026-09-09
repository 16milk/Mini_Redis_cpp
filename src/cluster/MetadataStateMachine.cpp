#include "mini_redis/cluster/MetadataStateMachine.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace cluster {
namespace {

constexpr char kSnapshotMagic[] = {'M', 'R', 'M', 'S'};
constexpr std::uint16_t kSnapshotVersion = 1;
constexpr std::size_t kMaxSnapshotBytes = 128U << 20;
constexpr std::size_t kMaxItems = 1U << 20;

class Writer {
public:
    void number(std::uint64_t value, unsigned width) {
        for (unsigned shift = width * 8; shift != 0; shift -= 8) {
            bytes_.push_back(static_cast<char>((value >> (shift - 8)) & 0xffU));
        }
    }
    void string(const std::string& value) {
        number(value.size(), 4);
        bytes_.append(value);
    }
    void endpoint(const Endpoint& value) {
        string(value.host);
        number(value.port, 2);
    }
    void nodes(const std::vector<NodeId>& values) {
        number(values.size(), 4);
        for (const NodeId& value : values) string(value);
    }
    std::string finish() { return std::move(bytes_); }

private:
    std::string bytes_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool number(unsigned width, std::uint64_t& value) {
        if (width > bytes_.size() - offset_) return false;
        value = 0;
        for (unsigned index = 0; index < width; ++index) {
            value = (value << 8) |
                    static_cast<unsigned char>(bytes_[offset_++]);
        }
        return true;
    }
    bool string(std::string& value) {
        std::uint64_t size = 0;
        if (!number(4, size) || size > bytes_.size() - offset_) return false;
        value.assign(bytes_.data() + offset_, static_cast<std::size_t>(size));
        offset_ += static_cast<std::size_t>(size);
        return true;
    }
    bool endpoint(Endpoint& value) {
        std::uint64_t port = 0;
        if (!string(value.host) || !number(2, port)) return false;
        value.port = static_cast<std::uint16_t>(port);
        return true;
    }
    bool nodes(std::vector<NodeId>& values) {
        std::uint64_t count = 0;
        if (!number(4, count) || count > kMaxItems) return false;
        values.clear();
        values.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            NodeId value;
            if (!string(value)) return false;
            values.push_back(std::move(value));
        }
        return true;
    }
    bool done() const { return offset_ == bytes_.size(); }

private:
    std::string_view bytes_;
    std::size_t offset_ = 0;
};

bool uniqueMembers(const std::vector<NodeId>& nodes) {
    std::set<NodeId> seen;
    for (const NodeId& node : nodes) {
        if (node.empty() || !seen.insert(node).second) return false;
    }
    return true;
}

bool contains(const std::vector<NodeId>& nodes, const NodeId& node) {
    return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

bool legalPhaseTransition(MigrationPhase from, MigrationPhase to) {
    switch (from) {
        case MigrationPhase::kPreparing:
            return to == MigrationPhase::kPrepared ||
                   to == MigrationPhase::kAborting;
        case MigrationPhase::kPrepared:
            return to == MigrationPhase::kCopyingBase ||
                   to == MigrationPhase::kAborting;
        case MigrationPhase::kCopyingBase:
            return to == MigrationPhase::kCatchingUp ||
                   to == MigrationPhase::kAborting;
        case MigrationPhase::kCatchingUp:
            return to == MigrationPhase::kSourceFenced ||
                   to == MigrationPhase::kAborting;
        case MigrationPhase::kSourceFenced:
            return to == MigrationPhase::kTargetActiveAsk;
        case MigrationPhase::kMetadataCommitted:
            return to == MigrationPhase::kCleanup;
        case MigrationPhase::kAborting:
            return to == MigrationPhase::kAborted;
        default:
            return false;
    }
}

bool validNodeStatus(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(MetadataNodeStatus::kJoining) &&
           value <= static_cast<std::uint64_t>(MetadataNodeStatus::kRemoved);
}

bool validSlotTransition(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(SlotTransition::kStable) &&
           value <= static_cast<std::uint64_t>(SlotTransition::kMigrating);
}

bool validMigrationPhase(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(MigrationPhase::kPreparing) &&
           value <= static_cast<std::uint64_t>(MigrationPhase::kAborted);
}

bool validApplyStatus(std::uint64_t value) {
    return value >= static_cast<std::uint64_t>(MetadataApplyStatus::kApplied) &&
           value <= static_cast<std::uint64_t>(MetadataApplyStatus::kStaleEpoch);
}

bool validOperation(MetadataOperation operation) {
    const auto value = static_cast<std::uint8_t>(operation);
    return value >=
               static_cast<std::uint8_t>(MetadataOperation::kInitializeCluster) &&
           value <=
               static_cast<std::uint8_t>(MetadataOperation::kFinishMigration);
}

} // namespace

MetadataApplyResult MetadataStateMachine::reject(MetadataApplyStatus status,
                                                 std::string error) const {
    MetadataApplyResult result;
    result.status = status;
    result.revision = metadata_.revision;
    result.error = std::move(error);
    return result;
}

MetadataApplyResult MetadataStateMachine::remember(const MetadataCommand& command,
                                                   MetadataApplyResult result) {
    DedupRecord record;
    record.encoded_command = encodeMetadataCommand(command);
    record.result = result;
    dedup_[command.request_id] = std::move(record);
    return result;
}

MetadataApplyResult MetadataStateMachine::apply(const consensus::LogEntry& entry) {
    if (entry.type != consensus::EntryType::kCommand) {
        return reject(MetadataApplyStatus::kInvalidCommand,
                      "metadata state machine accepts command entries only");
    }
    return apply(entry.payload);
}

MetadataApplyResult MetadataStateMachine::apply(const std::string& payload) {
    MetadataCommand command;
    std::string error;
    if (!decodeMetadataCommand(payload, command, error)) {
        return reject(MetadataApplyStatus::kInvalidCommand, std::move(error));
    }
    return apply(command);
}

MetadataApplyResult MetadataStateMachine::apply(const MetadataCommand& command) {
    if (command.version != kMetadataCommandVersion || command.request_id.empty() ||
        !validOperation(command.operation)) {
        return reject(MetadataApplyStatus::kInvalidCommand,
                      "invalid metadata command version or request id");
    }
    const std::string encoded = encodeMetadataCommand(command);
    const auto duplicate = dedup_.find(command.request_id);
    if (duplicate != dedup_.end()) {
        if (duplicate->second.encoded_command != encoded) {
            return reject(MetadataApplyStatus::kRequestConflict,
                          "request id was already used for a different command");
        }
        MetadataApplyResult result = duplicate->second.result;
        result.duplicate = true;
        return result;
    }
    if (command.expected_revision != metadata_.revision) {
        return remember(command, reject(MetadataApplyStatus::kCasMismatch,
                                        "metadata revision CAS mismatch"));
    }
    return remember(command, applyNew(command));
}

MetadataApplyResult MetadataStateMachine::applyNew(const MetadataCommand& command) {
    if (command.operation != MetadataOperation::kInitializeCluster &&
        !metadata_.initialized()) {
        return reject(MetadataApplyStatus::kInvalidCommand,
                      "cluster metadata is not initialized");
    }

    switch (command.operation) {
        case MetadataOperation::kInitializeCluster:
            if (metadata_.initialized() || command.cluster_id.empty()) {
                return reject(MetadataApplyStatus::kInvalidCommand,
                              "cluster is already initialized or cluster id is empty");
            }
            metadata_.cluster_id = command.cluster_id;
            break;

        case MetadataOperation::kUpsertNode: {
            const MetadataNodeRecord& node = command.node;
            if (node.id.empty() || node.client.empty() || node.internal.empty() ||
                node.admin.empty()) {
                return reject(MetadataApplyStatus::kInvalidCommand,
                              "node id and all advertised endpoints are required");
            }
            metadata_.nodes[node.id] = node;
            break;
        }

        case MetadataOperation::kRemoveNode:
            if (metadata_.nodes.find(command.node_id) == metadata_.nodes.end()) {
                return reject(MetadataApplyStatus::kNotFound, "node does not exist");
            }
            for (const auto& [id, group] : metadata_.groups) {
                (void)id;
                if (contains(group.voters, command.node_id) ||
                    contains(group.learners, command.node_id)) {
                    return reject(MetadataApplyStatus::kInvalidCommand,
                                  "node is still a member of a shard");
                }
            }
            metadata_.nodes.erase(command.node_id);
            break;

        case MetadataOperation::kUpsertGroup: {
            const MetadataGroupRecord& group = command.group;
            if (group.id == kNoShard || group.voters.empty() ||
                !uniqueMembers(group.voters) || !uniqueMembers(group.learners) ||
                !uniqueMembers(group.desired_placement)) {
                return reject(MetadataApplyStatus::kInvalidCommand,
                              "invalid shard membership");
            }
            for (const NodeId& voter : group.voters) {
                if (metadata_.nodes.find(voter) == metadata_.nodes.end() ||
                    contains(group.learners, voter)) {
                    return reject(MetadataApplyStatus::kInvalidCommand,
                                  "shard references unknown or duplicate member");
                }
            }
            for (const NodeId& learner : group.learners) {
                if (metadata_.nodes.find(learner) == metadata_.nodes.end()) {
                    return reject(MetadataApplyStatus::kInvalidCommand,
                                  "shard references unknown learner");
                }
            }
            metadata_.groups[group.id] = group;
            break;
        }

        case MetadataOperation::kRemoveGroup:
            if (metadata_.groups.find(command.group_id) == metadata_.groups.end()) {
                return reject(MetadataApplyStatus::kNotFound, "shard does not exist");
            }
            for (const MetadataSlotRecord& slot : metadata_.slots) {
                if (slot.active_group == command.group_id) {
                    return reject(MetadataApplyStatus::kInvalidCommand,
                                  "shard still owns slots");
                }
            }
            for (const auto& [id, migration] : metadata_.migrations) {
                (void)id;
                if (migration.source == command.group_id ||
                    migration.target == command.group_id) {
                    return reject(MetadataApplyStatus::kInvalidCommand,
                                  "shard participates in a migration");
                }
            }
            metadata_.groups.erase(command.group_id);
            break;

        case MetadataOperation::kAssignSlots:
            if (command.slot_start > command.slot_end ||
                command.slot_end >= kSlotCount ||
                metadata_.groups.find(command.owner_group) == metadata_.groups.end() ||
                command.new_ownership_epoch <= command.expected_ownership_epoch) {
                return reject(MetadataApplyStatus::kInvalidCommand,
                              "invalid slot assignment");
            }
            for (std::uint32_t slot = command.slot_start; slot <= command.slot_end;
                 ++slot) {
                const MetadataSlotRecord& current = metadata_.slots[slot];
                if (current.ownership_epoch != command.expected_ownership_epoch ||
                    current.transition != SlotTransition::kStable) {
                    return reject(MetadataApplyStatus::kStaleEpoch,
                                  "slot ownership epoch CAS mismatch");
                }
                if (current.active_group != kNoShard &&
                    current.active_group != command.owner_group) {
                    return reject(MetadataApplyStatus::kInvalidCommand,
                                  "slot owner changes require a migration proof");
                }
            }
            for (std::uint32_t slot = command.slot_start; slot <= command.slot_end;
                 ++slot) {
                MetadataSlotRecord& current = metadata_.slots[slot];
                current.active_group = command.owner_group;
                current.ownership_epoch = command.new_ownership_epoch;
            }
            break;

        case MetadataOperation::kBeginMigration: {
            if (command.slot_start != command.slot_end ||
                command.slot_start >= kSlotCount || command.migration_id.empty() ||
                command.source_group == kNoShard ||
                command.target_group == kNoShard ||
                command.source_group == command.target_group ||
                metadata_.migrations.find(command.migration_id) !=
                    metadata_.migrations.end() ||
                metadata_.groups.find(command.target_group) == metadata_.groups.end() ||
                command.expected_ownership_epoch ==
                    std::numeric_limits<std::uint64_t>::max() ||
                command.new_ownership_epoch !=
                    command.expected_ownership_epoch + 1) {
                return reject(MetadataApplyStatus::kInvalidCommand,
                              "invalid migration intent");
            }
            MetadataSlotRecord& slot = metadata_.slots[command.slot_start];
            if (slot.active_group != command.source_group ||
                slot.ownership_epoch != command.expected_ownership_epoch ||
                slot.transition != SlotTransition::kStable) {
                return reject(MetadataApplyStatus::kStaleEpoch,
                              "migration ownership CAS mismatch");
            }
            MetadataMigrationRecord migration;
            migration.id = command.migration_id;
            migration.slot = command.slot_start;
            migration.source = command.source_group;
            migration.target = command.target_group;
            migration.from_epoch = command.expected_ownership_epoch;
            migration.to_epoch = command.new_ownership_epoch;
            migration.phase = MigrationPhase::kPreparing;
            migration.progress_proof = command.progress_proof;
            metadata_.migrations[migration.id] = migration;
            slot.transition = SlotTransition::kMigrating;
            slot.migration_id = migration.id;
            break;
        }

        case MetadataOperation::kAdvanceMigration: {
            auto it = metadata_.migrations.find(command.migration_id);
            if (it == metadata_.migrations.end()) {
                return reject(MetadataApplyStatus::kNotFound,
                              "migration does not exist");
            }
            MetadataMigrationRecord& migration = it->second;
            const MetadataSlotRecord& slot = metadata_.slots[migration.slot];
            if (slot.ownership_epoch != command.expected_ownership_epoch) {
                return reject(MetadataApplyStatus::kStaleEpoch,
                              "migration epoch CAS mismatch");
            }
            if (!legalPhaseTransition(migration.phase, command.migration_phase)) {
                return reject(MetadataApplyStatus::kIllegalTransition,
                              "illegal migration phase transition");
            }
            migration.phase = command.migration_phase;
            migration.progress_proof = command.progress_proof;
            break;
        }

        case MetadataOperation::kCommitMigration: {
            auto it = metadata_.migrations.find(command.migration_id);
            if (it == metadata_.migrations.end()) {
                return reject(MetadataApplyStatus::kNotFound,
                              "migration does not exist");
            }
            MetadataMigrationRecord& migration = it->second;
            MetadataSlotRecord& slot = metadata_.slots[migration.slot];
            if (migration.phase != MigrationPhase::kTargetActiveAsk) {
                return reject(MetadataApplyStatus::kIllegalTransition,
                              "target is not active in ASK mode");
            }
            if (slot.active_group != migration.source ||
                slot.ownership_epoch != migration.from_epoch ||
                command.expected_ownership_epoch != migration.from_epoch) {
                return reject(MetadataApplyStatus::kStaleEpoch,
                              "owner changed before migration commit");
            }
            slot.active_group = migration.target;
            slot.ownership_epoch = migration.to_epoch;
            migration.phase = MigrationPhase::kMetadataCommitted;
            migration.progress_proof = command.progress_proof;
            break;
        }

        case MetadataOperation::kFinishMigration: {
            auto it = metadata_.migrations.find(command.migration_id);
            if (it == metadata_.migrations.end()) {
                return reject(MetadataApplyStatus::kNotFound,
                              "migration does not exist");
            }
            const MetadataMigrationRecord migration = it->second;
            if (migration.phase != MigrationPhase::kCleanup &&
                migration.phase != MigrationPhase::kAborted) {
                return reject(MetadataApplyStatus::kIllegalTransition,
                              "migration cannot be finished in this phase");
            }
            MetadataSlotRecord& slot = metadata_.slots[migration.slot];
            slot.transition = SlotTransition::kStable;
            slot.migration_id.clear();
            metadata_.migrations.erase(it);
            break;
        }
    }

    ++metadata_.revision;
    MetadataApplyResult result;
    result.status = MetadataApplyStatus::kApplied;
    result.revision = metadata_.revision;
    return result;
}

std::string MetadataStateMachine::snapshotBytes() const {
    Writer writer;
    for (char byte : kSnapshotMagic) writer.number(static_cast<unsigned char>(byte), 1);
    writer.number(kSnapshotVersion, 2);
    writer.string(metadata_.cluster_id);
    writer.number(metadata_.revision, 8);

    writer.number(metadata_.nodes.size(), 4);
    for (const auto& [id, node] : metadata_.nodes) {
        writer.string(id);
        writer.endpoint(node.client);
        writer.endpoint(node.internal);
        writer.endpoint(node.admin);
        writer.string(node.failure_domain);
        writer.number(static_cast<std::uint8_t>(node.status), 1);
    }

    writer.number(metadata_.groups.size(), 4);
    for (const auto& [id, group] : metadata_.groups) {
        writer.number(id, 4);
        writer.nodes(group.voters);
        writer.nodes(group.learners);
        writer.nodes(group.desired_placement);
        writer.number(group.config_index, 8);
    }

    for (const MetadataSlotRecord& slot : metadata_.slots) {
        writer.number(slot.active_group, 4);
        writer.number(slot.ownership_epoch, 8);
        writer.number(static_cast<std::uint8_t>(slot.transition), 1);
        writer.string(slot.migration_id);
    }

    writer.number(metadata_.migrations.size(), 4);
    for (const auto& [id, migration] : metadata_.migrations) {
        writer.string(id);
        writer.number(migration.slot, 2);
        writer.number(migration.source, 4);
        writer.number(migration.target, 4);
        writer.number(migration.from_epoch, 8);
        writer.number(migration.to_epoch, 8);
        writer.number(static_cast<std::uint8_t>(migration.phase), 1);
        writer.string(migration.progress_proof);
    }

    writer.number(dedup_.size(), 4);
    for (const auto& [request_id, record] : dedup_) {
        writer.string(request_id);
        writer.string(record.encoded_command);
        writer.number(static_cast<std::uint8_t>(record.result.status), 1);
        writer.number(record.result.revision, 8);
        writer.string(record.result.error);
    }
    return writer.finish();
}

bool MetadataStateMachine::installSnapshot(const std::string& bytes, std::string& error) {
    if (bytes.size() > kMaxSnapshotBytes || bytes.size() < sizeof(kSnapshotMagic) + 2) {
        error = "invalid metadata snapshot size";
        return false;
    }
    Reader reader(bytes);
    std::uint64_t value = 0;
    for (char expected : kSnapshotMagic) {
        if (!reader.number(1, value) ||
            value != static_cast<unsigned char>(expected)) {
            error = "invalid metadata snapshot header";
            return false;
        }
    }
    if (!reader.number(2, value) || value != kSnapshotVersion) {
        error = "unsupported metadata snapshot version";
        return false;
    }

    ClusterMetadata restored;
    std::map<std::string, DedupRecord> restored_dedup;
    if (!reader.string(restored.cluster_id) || !reader.number(8, restored.revision)) {
        error = "truncated metadata snapshot identity";
        return false;
    }

    std::uint64_t count = 0;
    if (!reader.number(4, count) || count > kMaxItems) return false;
    for (std::uint64_t index = 0; index < count; ++index) {
        MetadataNodeRecord node;
        if (!reader.string(node.id) || !reader.endpoint(node.client) ||
            !reader.endpoint(node.internal) || !reader.endpoint(node.admin) ||
            !reader.string(node.failure_domain) || !reader.number(1, value) ||
            !validNodeStatus(value)) {
            error = "truncated metadata node snapshot";
            return false;
        }
        node.status = static_cast<MetadataNodeStatus>(value);
        restored.nodes[node.id] = std::move(node);
    }

    if (!reader.number(4, count) || count > kMaxItems) return false;
    for (std::uint64_t index = 0; index < count; ++index) {
        MetadataGroupRecord group;
        if (!reader.number(4, value)) return false;
        group.id = static_cast<ShardId>(value);
        if (!reader.nodes(group.voters) || !reader.nodes(group.learners) ||
            !reader.nodes(group.desired_placement) ||
            !reader.number(8, group.config_index)) {
            error = "truncated metadata group snapshot";
            return false;
        }
        restored.groups[group.id] = std::move(group);
    }

    for (MetadataSlotRecord& slot : restored.slots) {
        if (!reader.number(4, value)) return false;
        slot.active_group = static_cast<ShardId>(value);
        if (!reader.number(8, slot.ownership_epoch) ||
            !reader.number(1, value) || !validSlotTransition(value)) return false;
        slot.transition = static_cast<SlotTransition>(value);
        if (!reader.string(slot.migration_id)) return false;
    }

    if (!reader.number(4, count) || count > kMaxItems) return false;
    for (std::uint64_t index = 0; index < count; ++index) {
        MetadataMigrationRecord migration;
        if (!reader.string(migration.id) || !reader.number(2, value)) return false;
        migration.slot = static_cast<SlotId>(value);
        if (!reader.number(4, value)) return false;
        migration.source = static_cast<ShardId>(value);
        if (!reader.number(4, value)) return false;
        migration.target = static_cast<ShardId>(value);
        if (!reader.number(8, migration.from_epoch) ||
            !reader.number(8, migration.to_epoch) ||
            !reader.number(1, value) || !validMigrationPhase(value)) return false;
        migration.phase = static_cast<MigrationPhase>(value);
        if (!reader.string(migration.progress_proof)) return false;
        restored.migrations[migration.id] = std::move(migration);
    }

    if (!reader.number(4, count) || count > kMaxItems) return false;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string request_id;
        DedupRecord record;
        if (!reader.string(request_id) ||
            !reader.string(record.encoded_command) ||
            !reader.number(1, value) || !validApplyStatus(value)) return false;
        record.result.status = static_cast<MetadataApplyStatus>(value);
        if (!reader.number(8, record.result.revision) ||
            !reader.string(record.result.error)) return false;
        restored_dedup[request_id] = std::move(record);
    }
    if (!reader.done() || restored.cluster_id.empty()) {
        error = "trailing or uninitialized metadata snapshot";
        return false;
    }
    metadata_ = std::move(restored);
    dedup_ = std::move(restored_dedup);
    error.clear();
    return true;
}

} // namespace cluster
