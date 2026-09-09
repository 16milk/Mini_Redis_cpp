#include "mini_redis/cluster/MetadataController.hpp"
#include "mini_redis/cluster/MetadataWatcher.hpp"
#include "mini_redis/cluster/SlotMigration.hpp"
#include "mini_redis/command/CanonicalCommand.hpp"
#include "mini_redis/consensus/MemoryStorage.hpp"
#include "mini_redis/core/Database.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr consensus::GroupId kSourceGroup = 1;
constexpr consensus::GroupId kTargetGroup = 2;
constexpr const char* kMigrationId = "migration-a";
constexpr std::int64_t kForever = std::numeric_limits<std::int64_t>::max();

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// Every shard and the metadata group share one key, exactly as they would share
// a cluster secret from configuration.
const cluster::MigrationKey& testMigrationKey() {
    static const cluster::MigrationKey key("cluster-migration-secret");
    return key;
}

cluster::SlotId migratingSlot() { return cluster::keyToSlot("mig"); }

std::string slotKey(const std::string& suffix) { return "{mig}:" + suffix; }

consensus::RaftOptions options() {
    consensus::RaftOptions result;
    result.heartbeat_interval_ms = 10;
    result.election_timeout_min_ms = 50;
    result.election_timeout_max_ms = 100;
    return result;
}

consensus::Configuration configuration() {
    consensus::Configuration result;
    result.incoming.voters = {"n1", "n2", "n3"};
    return consensus::normalizeConf(std::move(result));
}

cluster::MetadataNodeRecord metadataNode(const std::string& id, std::uint16_t port) {
    cluster::MetadataNodeRecord node;
    node.id = id;
    node.client = {"127.0.0.1", port};
    node.internal = {"127.0.0.1", static_cast<std::uint16_t>(port + 1000)};
    node.admin = {"127.0.0.1", static_cast<std::uint16_t>(port + 2000)};
    node.failure_domain = id;
    return node;
}

// One data shard's replica of the state machine: the keyspace, the committed
// slot ownership record and the migration protocol state, all advanced from the
// same Raft log.
struct ShardRuntime {
    Database database{true};
    cluster::SlotOwnershipTable ownership;
    cluster::SlotMigrationStateMachine migration;
    command::DeterministicStateMachine state_machine;
    command::ApplyResult last_write;
    cluster::MigrationApplyResult last_migration;

    explicit ShardRuntime(cluster::ShardId shard)
        : migration(database, ownership, shard, testMigrationKey()),
          state_machine(database, shard) {
        state_machine.setMigrationRecorder(&migration);
    }

    void applyEntry(const consensus::LogEntry& entry) {
        if (cluster::isSlotMigrationPayload(entry.payload)) {
            last_migration = migration.apply(entry);
        } else {
            last_write = state_machine.apply(entry.payload, ownership);
        }
    }

    // Stands in for the bootstrap entry a real deployment would commit when a
    // shard first takes ownership of a slot.
    void seedOwnership(cluster::SlotId slot, cluster::ShardId shard,
                       std::uint64_t epoch) {
        cluster::LocalSlotOwnership record;
        record.shard = shard;
        record.epoch = epoch;
        record.state = cluster::LocalSlotState::kStable;
        std::string error;
        expect(ownership.update(slot, 0, record, error), "seed slot ownership");
    }
};

struct Node {
    consensus::NodeId id;
    consensus::MultiRaft multi;
    std::map<consensus::GroupId, consensus::MemoryStorage> storage;
    std::map<consensus::GroupId, consensus::Index> applied;
    std::map<consensus::GroupId, std::map<consensus::ReadId, consensus::Index>> reads;
    std::map<consensus::GroupId, std::unique_ptr<ShardRuntime>> shards;
    cluster::ClusterRouter router;
    cluster::MetadataWatcher watcher;
    cluster::MetadataStateMachine metadata{testMigrationKey()};
    cluster::MetadataController controller;

    Node(consensus::NodeId node_id, const consensus::Configuration& conf,
         const consensus::RaftOptions& raft_options)
        : id(std::move(node_id)),
          multi(id, raft_options),
          router(id, {}),
          watcher(router),
          controller(multi, metadata, &watcher) {
        for (consensus::GroupId group :
             {consensus::kMetadataGroupId, kSourceGroup, kTargetGroup}) {
            storage.emplace(group, conf);
            applied[group] = 0;
            multi.createGroup(group, conf);
            if (group != consensus::kMetadataGroupId) {
                shards.emplace(group, std::make_unique<ShardRuntime>(
                                          static_cast<cluster::ShardId>(group)));
            }
        }
    }

    ShardRuntime& shard(consensus::GroupId group) { return *shards.at(group); }
};

class Cluster {
public:
    Cluster() {
        const consensus::Configuration conf = configuration();
        const consensus::RaftOptions raft_options = options();
        for (const char* id : {"n1", "n2", "n3"}) {
            nodes_.push_back(std::make_unique<Node>(id, conf, raft_options));
        }
    }

    Node& node(const consensus::NodeId& id) {
        for (auto& node : nodes_) {
            if (node->id == id) return *node;
        }
        expect(false, "unknown node " + id);
        std::abort();
    }

    std::vector<std::unique_ptr<Node>>& nodes() { return nodes_; }

    consensus::RaftCore* leader(consensus::GroupId group) {
        for (auto& node : nodes_) {
            consensus::RaftCore& core = node->multi.group(group);
            if (core.role() == consensus::RaftRole::kLeader) return &core;
        }
        return nullptr;
    }

    Node& leaderNode(consensus::GroupId group) {
        consensus::RaftCore* core = leader(group);
        expect(core != nullptr, "group has a leader");
        return node(core->selfId());
    }

    void elect(consensus::GroupId group) {
        for (int round = 0; round < 200 && leader(group) == nullptr; ++round) {
            tick(10);
        }
        expect(leader(group) != nullptr, "group elects a leader");
    }

    void tick(std::int64_t delta_ms) {
        pump();
        now_ms_ += delta_ms;
        for (auto& node : nodes_) node->multi.tick(now_ms_);
        pump();
    }

    void pump() {
        for (int round = 0; round < 400; ++round) {
            bool progress = false;
            for (auto& node : nodes_) {
                for (consensus::GroupId group : node->multi.groupIds()) {
                    consensus::RaftCore& core = node->multi.group(group);
                    while (core.hasReady()) {
                        consensus::Ready ready = core.takeReady();
                        node->storage.at(group).applyReady(ready);
                        for (const consensus::LogEntry& entry : ready.committed) {
                            if (entry.type == consensus::EntryType::kCommand) {
                                if (group == consensus::kMetadataGroupId) {
                                    node->controller.applyCommitted(entry);
                                } else {
                                    node->shard(group).applyEntry(entry);
                                }
                            }
                            node->applied[group] = entry.index;
                        }
                        if (node->applied[group] > 0) {
                            core.reportApplied(node->applied[group]);
                        }
                        for (const consensus::ReadyRead& read : ready.reads) {
                            node->reads[group][read.id] = read.read_index;
                        }
                        for (consensus::Message& message : ready.messages) {
                            mailbox_.push_back(std::move(message));
                        }
                        core.advance();
                        progress = true;
                    }
                }
            }
            if (!mailbox_.empty()) {
                std::vector<consensus::Message> batch;
                batch.swap(mailbox_);
                for (const consensus::Message& message : batch) {
                    node(message.to).multi.step(message);
                }
                progress = true;
            }
            if (!progress) return;
        }
        expect(false, "cluster pump becomes idle");
    }

    // Waits for every replica, not just the leader: the assertions in this test
    // are about replicated state, not about what one node happens to know.
    bool awaitApplied(consensus::GroupId group, consensus::Index index) {
        for (int round = 0; round < 60; ++round) {
            pump();
            bool applied_everywhere = true;
            for (auto& node : nodes_) {
                applied_everywhere &= node->applied[group] >= index;
            }
            if (applied_everywhere) return true;
            tick(10);
        }
        return false;
    }

    bool submitMetadata(cluster::MetadataCommand command) {
        Node& owner = leaderNode(consensus::kMetadataGroupId);
        command.expected_revision = owner.metadata.metadata().revision;
        const std::uint64_t target = command.expected_revision + 1;
        const consensus::ProposeResult proposed = owner.controller.propose(command);
        if (proposed.error != consensus::ProposeError::kOk) return false;
        for (int round = 0; round < 60; ++round) {
            pump();
            if (owner.metadata.metadata().revision >= target) {
                settle();
                return true;
            }
            tick(10);
        }
        return false;
    }

    // Proposes an already canonicalized write to a data group and returns the
    // state machine result observed by the leader replica.
    command::ApplyResult write(consensus::GroupId group,
                               const std::vector<std::string>& request,
                               std::uint64_t epoch) {
        command::CanonicalizeContext context;
        context.shard = static_cast<cluster::ShardId>(group);
        context.slot_epoch = epoch;
        context.logical_time_ms = nextLogicalTime();
        const command::CanonicalizeResult canonical =
            command::canonicalizeWrite(request, context);
        expect(canonical.ok, "write canonicalizes: " + canonical.error);

        Node& owner = leaderNode(group);
        const consensus::ProposeResult proposed = owner.multi.group(group).propose(
            command::encodeCanonicalCommand(canonical.command));
        expect(proposed.error == consensus::ProposeError::kOk, "write proposes");
        expect(awaitApplied(group, proposed.index), "write commits");
        return owner.shard(group).last_write;
    }

    cluster::MigrationApplyResult submitMigration(
        consensus::GroupId group, cluster::SlotMigrationCommand command) {
        command.slot = migratingSlot();
        command.source = static_cast<cluster::ShardId>(kSourceGroup);
        command.target = static_cast<cluster::ShardId>(kTargetGroup);
        command.migration_id = kMigrationId;
        command.from_epoch = 1;
        command.to_epoch = 2;
        if (command.logical_time_ms == 0) command.logical_time_ms = nextLogicalTime();

        Node& owner = leaderNode(group);
        const consensus::ProposeResult proposed = owner.multi.group(group).propose(
            cluster::encodeSlotMigrationCommand(command));
        expect(proposed.error == consensus::ProposeError::kOk, "migration proposes");
        expect(awaitApplied(group, proposed.index), "migration commits");
        return owner.shard(group).last_migration;
    }

    // A ReadIndex barrier that has been acknowledged by a quorum, which is what
    // proves the node was still the leader when it signed the fence.
    std::optional<consensus::Index> linearizableRead(consensus::GroupId group) {
        consensus::RaftCore* core = leader(group);
        if (core == nullptr) return std::nullopt;
        const consensus::NodeId owner_id = core->selfId();
        const consensus::ReadRequest request = core->readIndex();
        if (request.error != consensus::ReadError::kOk) return std::nullopt;
        for (int round = 0; round < 60; ++round) {
            pump();
            const auto& group_reads = node(owner_id).reads[group];
            const auto found = group_reads.find(request.id);
            if (found != group_reads.end()) return found->second;
            tick(10);
        }
        return std::nullopt;
    }

    void transferLeadership(consensus::GroupId group) {
        consensus::RaftCore* core = leader(group);
        expect(core != nullptr, "group has a leader before transfer");
        const consensus::NodeId previous = core->selfId();
        consensus::NodeId target;
        for (auto& node : nodes_) {
            if (node->id != previous) {
                target = node->id;
                break;
            }
        }
        expect(core->transferLeadership(target), "leadership transfer starts");
        for (int round = 0; round < 200; ++round) {
            tick(10);
            consensus::RaftCore* current = leader(group);
            if (current != nullptr && current->selfId() != previous) return;
        }
        expect(false, "leadership moves to another node");
    }

    void settle() {
        for (int round = 0; round < 60; ++round) {
            pump();
            const std::uint64_t revision = nodes_.front()->metadata.metadata().revision;
            bool converged = true;
            for (auto& node : nodes_) {
                converged &= node->metadata.metadata().revision == revision;
            }
            if (converged) return;
            tick(10);
        }
        expect(false, "committed metadata reaches every node");
    }

    // What a node publishes to its router: ownership from committed metadata,
    // leader hints from the live Raft groups.
    void publishTopology() {
        settle();
        const cluster::ClusterMetadata& metadata = nodes_.front()->metadata.metadata();
        cluster::TopologyBuilder builder;
        builder.setTopologyRevision(metadata.revision);
        for (const auto& [id, record] : metadata.nodes) {
            builder.addNode(id, record.client.host, record.client.port);
        }
        for (const auto& [id, group] : metadata.groups) {
            builder.addShard(id, group.voters);
            consensus::RaftCore* core = leader(id);
            if (core != nullptr) {
                builder.setLeaderHint(id, core->selfId(), core->term(), kForever);
            }
        }
        for (std::size_t slot = 0; slot < metadata.slots.size(); ++slot) {
            const cluster::MetadataSlotRecord& record = metadata.slots[slot];
            if (record.active_group == cluster::kNoShard) continue;
            builder.assignSlot(record.active_group, static_cast<cluster::SlotId>(slot));
            builder.setSlotEpoch(static_cast<cluster::SlotId>(slot),
                                 record.ownership_epoch);
        }
        for (const auto& [id, migration] : metadata.migrations) {
            (void)id;
            if (migration.phase == cluster::MigrationPhase::kMetadataCommitted ||
                migration.phase == cluster::MigrationPhase::kCleanup ||
                migration.phase == cluster::MigrationPhase::kAborting ||
                migration.phase == cluster::MigrationPhase::kAborted) {
                continue;
            }
            builder.setMigration(
                migration.slot, migration.source, migration.target,
                migration.phase == cluster::MigrationPhase::kTargetActiveAsk,
                migration.from_epoch, migration.to_epoch);
        }
        const cluster::TopologyPtr snapshot = builder.build();
        for (auto& node : nodes_) {
            expect(node->router.publishTopology(snapshot), "topology publishes");
        }
    }

    std::int64_t nextLogicalTime() { return logical_time_ms_ += 1000; }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<consensus::Message> mailbox_;
    std::int64_t now_ms_ = 0;
    std::int64_t logical_time_ms_ = 1'700'000'000'000;
};

cluster::MetadataCommand metadataCommand(cluster::MetadataOperation operation,
                                         std::string request_id) {
    cluster::MetadataCommand command;
    command.operation = operation;
    command.request_id = std::move(request_id);
    return command;
}

cluster::MetadataCommand advanceTo(cluster::MigrationPhase phase,
                                   std::string request_id, std::uint64_t epoch,
                                   std::string proof = {},
                                   std::string activation = {}) {
    cluster::MetadataCommand command =
        metadataCommand(cluster::MetadataOperation::kAdvanceMigration,
                        std::move(request_id));
    command.migration_id = kMigrationId;
    command.expected_ownership_epoch = epoch;
    command.migration_phase = phase;
    command.progress_proof = std::move(proof);
    command.activation_proof = std::move(activation);
    return command;
}

cluster::SlotMigrationCommand migrationOp(cluster::MigrationLogOp op) {
    cluster::SlotMigrationCommand command;
    command.op = op;
    return command;
}

// --- standalone protocol checks --------------------------------------------

void testCodecsAndDigest() {
    cluster::MigrationCommitProof proof;
    proof.migration_id = kMigrationId;
    proof.slot = migratingSlot();
    proof.source = 1;
    proof.target = 2;
    proof.from_epoch = 4;
    proof.to_epoch = 5;
    proof.fence_term = 3;
    proof.fence_index = 77;
    proof.read_index = 80;
    proof.fence_sequence = 9;
    proof.logical_time_ms = 1234;
    proof.digest = "abc";

    cluster::MigrationCommitProof decoded;
    std::string error;
    const std::string signed_proof =
        cluster::encodeCommitProof(proof, testMigrationKey());
    expect(cluster::decodeCommitProof(signed_proof, testMigrationKey(), decoded,
                                      error) &&
               decoded.fence_index == 77 && decoded.read_index == 80 &&
               decoded.digest == "abc",
           "commit proof codec round trip");
    expect(!cluster::decodeCommitProof(signed_proof + "x", testMigrationKey(),
                                       decoded, error),
           "commit proof codec rejects trailing bytes");

    // The signature is what stops anything outside the cluster from minting a
    // fence that never happened.
    expect(!cluster::decodeCommitProof(signed_proof,
                                       cluster::MigrationKey("wrong-secret"),
                                       decoded, error),
           "a proof signed with another key does not verify");
    std::string tampered_body = signed_proof;
    tampered_body[tampered_body.size() - cluster::kMigrationTagBytes - 1] ^= 0x01;
    expect(!cluster::decodeCommitProof(tampered_body, testMigrationKey(), decoded,
                                       error),
           "flipping a byte of the signed body invalidates the proof");
    expect(!cluster::decodeCommitProof(signed_proof, cluster::MigrationKey(),
                                       decoded, error),
           "a shard with no key configured accepts no proof at all");
    expect(cluster::validateCommitProof(proof, error), "well formed proof validates");

    cluster::MigrationCommitProof no_barrier = proof;
    no_barrier.read_index = 76;
    expect(!cluster::validateCommitProof(no_barrier, error),
           "a read barrier before the fence is not a proof");
    cluster::MigrationCommitProof no_fence = proof;
    no_fence.fence_index = 0;
    expect(!cluster::validateCommitProof(no_fence, error),
           "a proof without a fence entry is rejected");
    cluster::MigrationCommitProof reused_epoch = proof;
    reused_epoch.to_epoch = reused_epoch.from_epoch;
    expect(!cluster::validateCommitProof(reused_epoch, error),
           "a proof that does not advance the epoch is rejected");

    cluster::SlotMigrationCommand command = migrationOp(cluster::MigrationLogOp::kStageDelta);
    command.migration_id = kMigrationId;
    command.slot = migratingSlot();
    command.source = 1;
    command.target = 2;
    command.from_epoch = 1;
    command.to_epoch = 2;
    command.records.push_back({7, slotKey("a"), true, "payload", 42});
    command.records.push_back({8, slotKey("b"), false, "", 0});
    cluster::MigrationActivationProof activation;
    activation.migration_id = kMigrationId;
    activation.slot = migratingSlot();
    activation.source = 1;
    activation.target = 2;
    activation.from_epoch = 4;
    activation.to_epoch = 5;
    activation.activate_term = 2;
    activation.activate_index = 40;
    activation.read_index = 44;
    activation.digest = "abc";
    cluster::MigrationActivationProof decoded_activation;
    expect(cluster::decodeActivationProof(
               cluster::encodeActivationProof(activation, testMigrationKey()),
               testMigrationKey(), decoded_activation, error) &&
               decoded_activation.activate_index == 40 &&
               decoded_activation.read_index == 44,
           "activation proof codec round trip");
    expect(cluster::validateActivationProof(activation, error),
           "well formed activation proof validates");
    cluster::MigrationActivationProof unbarriered_activation = activation;
    unbarriered_activation.read_index = 39;
    expect(!cluster::validateActivationProof(unbarriered_activation, error),
           "an activation proof whose barrier precedes the activation is rejected");
    expect(!cluster::decodeCommitProof(
               cluster::encodeActivationProof(activation, testMigrationKey()),
               testMigrationKey(), decoded, error),
           "an activation proof is not accepted as a fence proof");

    cluster::SlotMigrationCommand decoded_command;
    const std::string encoded = cluster::encodeSlotMigrationCommand(command);
    expect(cluster::isSlotMigrationPayload(encoded),
           "migration payloads are distinguishable from canonical writes");
    expect(cluster::decodeSlotMigrationCommand(encoded, decoded_command, error) &&
               decoded_command.records.size() == 2 &&
               decoded_command.records[0].key == slotKey("a") &&
               decoded_command.records[1].present == false,
           "migration command codec round trip");

    std::vector<cluster::SlotDataRecord> records;
    records.push_back({0, slotKey("a"), true, "one", 0});
    records.push_back({0, slotKey("b"), true, "two", 0});
    records.push_back({0, slotKey("c"), true, "three", 5000});
    const std::string digest = cluster::slotDigest(records, 1000);
    std::reverse(records.begin(), records.end());
    expect(cluster::slotDigest(records, 1000) == digest,
           "the digest does not depend on record order");

    records.push_back({0, slotKey("d"), false, "", 0});
    expect(cluster::slotDigest(records, 1000) == digest,
           "tombstones do not change the digest");
    expect(cluster::slotDigest(records, 6000) != digest,
           "a key that has logically expired changes the digest");
    records.back().present = true;
    records.back().value = "four";
    expect(cluster::slotDigest(records, 1000) != digest,
           "an extra live key changes the digest");
}

// A source shard outside Raft, used for the negative paths that would otherwise
// desynchronize the replicated cluster.
struct StandaloneShard {
    Database database{true};
    cluster::SlotOwnershipTable ownership;
    cluster::SlotMigrationStateMachine migration;

    explicit StandaloneShard(cluster::ShardId shard)
        : migration(database, ownership, shard, testMigrationKey()) {}

    void own(cluster::SlotId slot, cluster::ShardId shard, std::uint64_t epoch) {
        cluster::LocalSlotOwnership record;
        record.shard = shard;
        record.epoch = epoch;
        record.state = cluster::LocalSlotState::kStable;
        std::string error;
        expect(ownership.update(slot, 0, record, error), "standalone ownership");
    }

    cluster::MigrationApplyResult apply(cluster::SlotMigrationCommand command,
                                        consensus::Index index) {
        command.migration_id = kMigrationId;
        command.slot = migratingSlot();
        command.source = 1;
        command.target = 2;
        command.from_epoch = 1;
        command.to_epoch = 2;
        if (command.logical_time_ms == 0) command.logical_time_ms = 1'700'000'000'000;
        return migration.apply(command, index, 1);
    }
};

void testFenceIsOneWay() {
    StandaloneShard source(1);
    const cluster::SlotId slot = migratingSlot();
    source.own(slot, 1, 1);
    source.database.set(slotKey("a"), "v");

    expect(source.apply(migrationOp(cluster::MigrationLogOp::kSourceBegin), 10).status ==
               cluster::MigrationApplyStatus::kApplied,
           "source begins the migration");
    expect(source.apply(migrationOp(cluster::MigrationLogOp::kAbort), 11).status ==
               cluster::MigrationApplyStatus::kApplied,
           "a copying migration can still be abandoned");

    StandaloneShard fenced(1);
    fenced.own(slot, 1, 1);
    fenced.database.set(slotKey("a"), "v");
    expect(fenced.apply(migrationOp(cluster::MigrationLogOp::kSourceBegin), 10).status ==
               cluster::MigrationApplyStatus::kApplied,
           "source begins the migration again");
    const cluster::MigrationApplyResult fence_result =
        fenced.apply(migrationOp(cluster::MigrationLogOp::kSourceFence), 20);
    expect(fence_result.status == cluster::MigrationApplyStatus::kApplied,
           "source fences the slot");

    expect(fenced.apply(migrationOp(cluster::MigrationLogOp::kAbort), 21).status ==
               cluster::MigrationApplyStatus::kIllegalTransition,
           "a fenced migration can no longer be abandoned");

    // No amount of waiting reopens the old epoch: the ownership record itself
    // refuses to return to a writable state at the same epoch.
    cluster::LocalSlotOwnership reopened = fenced.ownership.get(slot);
    reopened.state = cluster::LocalSlotState::kStable;
    std::string error;
    expect(!fenced.ownership.update(slot, 1, reopened, error),
           "a fenced source cannot reopen the old epoch after a timeout");
    expect(!fenced.ownership.canAcceptWrite(slot, 1, 1, false),
           "a fenced source accepts no further writes");
    expect(!fenced.ownership.canAcceptWrite(slot, 1, 1, true),
           "ASKING does not bypass the fence on the source");

    // Re-proposing the fence must not move the fence index or the digest, or the
    // proof already handed to the control plane would stop matching.
    const cluster::MigrationApplyResult replayed =
        fenced.apply(migrationOp(cluster::MigrationLogOp::kSourceFence), 33);
    expect(replayed.status == cluster::MigrationApplyStatus::kDuplicate,
           "re-proposing the fence is idempotent");
    const cluster::LocalMigrationState* state = fenced.migration.find(kMigrationId);
    expect(state != nullptr && state->fence_index == 20,
           "the fence index is pinned by the first commit");

    cluster::MigrationCommitProof proof;
    expect(fenced.migration.buildCommitProof(kMigrationId, 20, proof, error),
           "a barrier at the fence index proves the fence");
    expect(!fenced.migration.buildCommitProof(kMigrationId, 19, proof, error),
           "a barrier taken before the fence proves nothing");
}

void testTargetRefusesUnsafeActivation() {
    const cluster::SlotId slot = migratingSlot();

    StandaloneShard source(1);
    source.own(slot, 1, 1);
    source.database.set(slotKey("a"), "one");
    source.database.set(slotKey("b"), "two");
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceBegin), 10);
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceFence), 20);
    cluster::MigrationCommitProof proof;
    std::string error;
    expect(source.migration.buildCommitProof(kMigrationId, 25, proof, error),
           "source builds a proof");

    StandaloneShard target(2);
    expect(target.apply(migrationOp(cluster::MigrationLogOp::kTargetBegin), 5).status ==
               cluster::MigrationApplyStatus::kApplied,
           "target opens a staging space");

    cluster::SlotMigrationCommand activate =
        migrationOp(cluster::MigrationLogOp::kTargetActivate);
    activate.commit_proof = cluster::encodeCommitProof(proof, testMigrationKey());
    expect(target.apply(activate, 6).status ==
               cluster::MigrationApplyStatus::kIllegalTransition,
           "the target refuses to activate before the base snapshot completes");

    bool complete = false;
    cluster::SlotMigrationCommand base = migrationOp(cluster::MigrationLogOp::kStageBase);
    base.records = source.migration.baseChunkAfter(kMigrationId, "", 100, complete);
    base.base_complete = complete;
    expect(complete && base.records.size() == 2, "the base snapshot has both keys");
    expect(target.apply(base, 7).status == cluster::MigrationApplyStatus::kApplied,
           "the base snapshot stages");
    expect(target.database.physicalKeyCount() == 0,
           "staged data is invisible in the keyspace before activation");

    cluster::SlotMigrationCommand tampered = activate;
    cluster::MigrationCommitProof wrong_digest = proof;
    wrong_digest.digest = "00000000000000000000000000000000";
    tampered.commit_proof = cluster::encodeCommitProof(wrong_digest, testMigrationKey());
    expect(target.apply(tampered, 8).status ==
               cluster::MigrationApplyStatus::kDigestMismatch,
           "the target refuses a proof whose digest it cannot reproduce");

    cluster::MigrationCommitProof ahead = proof;
    ahead.fence_sequence = 4;
    cluster::SlotMigrationCommand impatient = activate;
    impatient.commit_proof = cluster::encodeCommitProof(ahead, testMigrationKey());
    expect(target.apply(impatient, 9).status ==
               cluster::MigrationApplyStatus::kSequenceGap,
           "the target refuses to activate before it reaches the fence sequence");

    cluster::MigrationCommitProof foreign = proof;
    foreign.migration_id = "other";
    cluster::SlotMigrationCommand mismatched = activate;
    mismatched.commit_proof = cluster::encodeCommitProof(foreign, testMigrationKey());
    expect(target.apply(mismatched, 10).status ==
               cluster::MigrationApplyStatus::kProofRejected,
           "the target refuses a proof for a different migration");

    expect(target.apply(activate, 11).status == cluster::MigrationApplyStatus::kApplied,
           "a matching proof activates the slot");
    cluster::MigrationActivationProof activation;
    expect(!target.migration.buildActivationProof(kMigrationId, 10, activation, error),
           "a barrier taken before the activation proves nothing");
    expect(target.migration.buildActivationProof(kMigrationId, 11, activation, error) &&
               activation.digest == proof.digest && activation.activate_index == 11,
           "the target can prove the activation it committed");
    std::string value;
    expect(target.database.get(slotKey("a"), value) && value == "one",
           "activation promotes staging into the live keyspace");
    expect(target.ownership.get(slot).state ==
               cluster::LocalSlotState::kTargetActiveAsk,
           "an activated target starts in ASK-only mode");
    expect(!target.ownership.canAcceptWrite(slot, 2, 2, false),
           "an ASK-only target rejects writes that did not go through ASKING");
    expect(target.ownership.canAcceptWrite(slot, 2, 2, true),
           "an ASK-only target accepts writes behind ASKING");
    expect(target.apply(activate, 12).status == cluster::MigrationApplyStatus::kDuplicate,
           "re-proposing the activation is idempotent");
}

void testDeltaJournalRequiresContiguity() {
    StandaloneShard target(2);
    target.apply(migrationOp(cluster::MigrationLogOp::kTargetBegin), 1);

    cluster::SlotMigrationCommand delta = migrationOp(cluster::MigrationLogOp::kStageDelta);
    delta.records.push_back({1, slotKey("a"), true, "", 0});
    expect(target.apply(delta, 2).status ==
               cluster::MigrationApplyStatus::kIllegalTransition,
           "deltas cannot precede a complete base snapshot");

    cluster::SlotMigrationCommand base = migrationOp(cluster::MigrationLogOp::kStageBase);
    base.base_complete = true;
    expect(target.apply(base, 3).status == cluster::MigrationApplyStatus::kApplied,
           "an empty base snapshot completes");

    cluster::SlotMigrationCommand gap = migrationOp(cluster::MigrationLogOp::kStageDelta);
    gap.records.push_back({2, slotKey("a"), true, "", 0});
    expect(target.apply(gap, 4).status == cluster::MigrationApplyStatus::kSequenceGap,
           "a delta batch that skips a sequence is rejected");

    cluster::SlotMigrationCommand foreign = migrationOp(cluster::MigrationLogOp::kStageDelta);
    foreign.records.push_back({1, "unrelated-key", true, "", 0});
    expect(target.apply(foreign, 5).status ==
               cluster::MigrationApplyStatus::kInvalidCommand,
           "a delta batch cannot carry keys from another slot");

    expect(target.apply(delta, 6).status == cluster::MigrationApplyStatus::kApplied,
           "the first delta applies");
    expect(target.apply(delta, 7).status == cluster::MigrationApplyStatus::kDuplicate,
           "replaying a staged delta batch changes nothing");
}

void testSnapshotResumesMidFlight() {
    const cluster::SlotId slot = migratingSlot();
    StandaloneShard source(1);
    source.own(slot, 1, 1);
    source.database.set(slotKey("a"), "one");
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceBegin), 10);
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceFence), 20);

    StandaloneShard restarted(1);
    restarted.own(slot, 1, 1);
    std::string error;
    expect(restarted.migration.installSnapshot(source.migration.snapshotBytes(), error),
           "a migration snapshot installs: " + error);

    const cluster::LocalMigrationState* before = source.migration.find(kMigrationId);
    const cluster::LocalMigrationState* after = restarted.migration.find(kMigrationId);
    expect(before != nullptr && after != nullptr, "both sides know the migration");
    expect(after->stage == before->stage && after->fence_index == before->fence_index &&
               after->fence_term == before->fence_term &&
               after->fence_sequence == before->fence_sequence &&
               after->fence_digest == before->fence_digest &&
               after->base.size() == before->base.size(),
           "a restarted replica resumes from the committed fence");

    cluster::MigrationCommitProof original;
    cluster::MigrationCommitProof resumed;
    expect(source.migration.buildCommitProof(kMigrationId, 30, original, error) &&
               restarted.migration.buildCommitProof(kMigrationId, 30, resumed, error),
           "both replicas can produce the proof");
    expect(cluster::encodeCommitProof(original, testMigrationKey()) ==
               cluster::encodeCommitProof(resumed, testMigrationKey()),
           "the proof survives a restart byte for byte");

    StandaloneShard other_shard(2);
    expect(!other_shard.migration.installSnapshot(source.migration.snapshotBytes(), error),
           "a migration snapshot does not install into the wrong shard");
}

// Deleting a key the target has never been told about produces a tombstone that
// would apply to nothing. Journaling it anyway lets a loop of deletes against
// absent keys grow the journal without bound, and the journal is retained until
// the slot is released.
void testJournalIgnoresVacuousDeletes() {
    const cluster::SlotId slot = migratingSlot();
    StandaloneShard source(1);
    source.own(slot, 1, 1);
    source.database.set(slotKey("real"), "value");
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceBegin), 10);

    const cluster::LocalMigrationState* state = source.migration.find(kMigrationId);
    expect(state != nullptr && state->base.size() == 1, "the base holds one key");

    for (int attempt = 0; attempt < 100; ++attempt) {
        source.migration.captureWrite(slot, {slotKey("never-existed")}, 1000);
    }
    expect(state->deltas.empty(),
           "deleting a key the target never had is not worth a delta");
    expect(state->next_sequence == 1,
           "and it does not consume a sequence number either");

    // A key that is in the base does need its tombstone: the target has it.
    source.database.del({slotKey("real")});
    source.migration.captureWrite(slot, {slotKey("real")}, 1000);
    expect(state->deltas.size() == 1 && !state->deltas.begin()->second.present,
           "deleting a key the target does hold is journaled");

    // Once a key has been journaled as present, a later delete matters again.
    source.database.set(slotKey("new"), "v");
    source.migration.captureWrite(slot, {slotKey("new")}, 1000);
    source.database.del({slotKey("new")});
    source.migration.captureWrite(slot, {slotKey("new")}, 1000);
    expect(state->deltas.size() == 3,
           "a key created during the copy is journaled, and so is its delete");

    // The suppression rule is replicated state, so a replica that installs the
    // snapshot has to reach the same decisions.
    StandaloneShard restarted(1);
    restarted.own(slot, 1, 1);
    std::string error;
    expect(restarted.migration.installSnapshot(source.migration.snapshotBytes(), error),
           "the journal snapshot installs: " + error);
    restarted.migration.captureWrite(slot, {slotKey("never-existed")}, 1000);
    source.migration.captureWrite(slot, {slotKey("never-existed")}, 1000);
    expect(restarted.migration.find(kMigrationId)->deltas.size() ==
               state->deltas.size(),
           "a restarted replica suppresses exactly the same tombstones");
}

// A fenced source must not be reopened, and a released one must leave behind an
// epoch that a later migration cannot reuse.
void testReleaseLeavesTheEpochAdvanced() {
    const cluster::SlotId slot = migratingSlot();
    StandaloneShard source(1);
    source.own(slot, 1, 1);
    source.database.set(slotKey("a"), "one");
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceBegin), 10);
    source.apply(migrationOp(cluster::MigrationLogOp::kSourceFence), 20);

    cluster::MigrationCommitProof fence;
    std::string error;
    expect(source.migration.buildCommitProof(kMigrationId, 25, fence, error),
           "the source proves its fence");

    cluster::MigrationActivationProof activation;
    activation.migration_id = kMigrationId;
    activation.slot = slot;
    activation.source = 1;
    activation.target = 2;
    activation.from_epoch = 1;
    activation.to_epoch = 2;
    activation.activate_term = 1;
    activation.activate_index = 30;
    activation.read_index = 30;
    activation.digest = fence.digest;

    cluster::SlotMigrationCommand release =
        migrationOp(cluster::MigrationLogOp::kSourceRelease);
    release.activation_proof =
        cluster::encodeActivationProof(activation, testMigrationKey());
    expect(source.apply(release, 40).status == cluster::MigrationApplyStatus::kApplied,
           "the proven release applies");

    // Not from_epoch: ownership demonstrably reached to_epoch, and recording the
    // old number would let a migration back into this shard reuse an epoch the
    // cluster has already moved past.
    const cluster::LocalSlotOwnership& released = source.ownership.get(slot);
    expect(released.state == cluster::LocalSlotState::kUnowned &&
               released.shard == cluster::kNoShard && released.epoch == 2,
           "the released slot keeps the epoch ownership advanced to");

    // Applied directly, because this command deliberately reverses the roles
    // the StandaloneShard helper would otherwise stamp on it.
    cluster::SlotMigrationCommand reuse =
        migrationOp(cluster::MigrationLogOp::kTargetBegin);
    reuse.migration_id = "migration-b";
    reuse.slot = slot;
    reuse.source = 2;
    reuse.target = 1;
    reuse.from_epoch = 1;
    reuse.to_epoch = 2;
    reuse.logical_time_ms = 1'700'000'000'000;
    expect(source.migration.apply(reuse, 50, 1).status ==
               cluster::MigrationApplyStatus::kStaleEpoch,
           "the slot cannot be taken back at an epoch already spent");

    cluster::SlotMigrationCommand fresh = reuse;
    fresh.migration_id = "migration-c";
    fresh.from_epoch = 2;
    fresh.to_epoch = 3;
    expect(source.migration.apply(fresh, 51, 1).status ==
               cluster::MigrationApplyStatus::kApplied,
           "but it can be taken back at a strictly newer one");
}

// --- full online migration over Raft ----------------------------------------

void testOnlineMigration() {
    Cluster cluster;
    const cluster::SlotId slot = migratingSlot();
    cluster.elect(consensus::kMetadataGroupId);
    cluster.elect(kSourceGroup);
    cluster.elect(kTargetGroup);

    cluster::MetadataCommand initialize =
        metadataCommand(cluster::MetadataOperation::kInitializeCluster, "init");
    initialize.cluster_id = "migration-cluster";
    expect(cluster.submitMetadata(initialize), "cluster initializes");
    for (int index = 1; index <= 3; ++index) {
        cluster::MetadataCommand add = metadataCommand(
            cluster::MetadataOperation::kUpsertNode, "node-" + std::to_string(index));
        add.node = metadataNode("n" + std::to_string(index),
                                static_cast<std::uint16_t>(7000 + index));
        expect(cluster.submitMetadata(add), "node registers");
    }
    for (cluster::ShardId shard : {1U, 2U}) {
        cluster::MetadataCommand add =
            metadataCommand(cluster::MetadataOperation::kUpsertGroup,
                            "group-" + std::to_string(shard));
        add.group.id = shard;
        add.group.voters = {"n1", "n2", "n3"};
        expect(cluster.submitMetadata(add), "shard registers");
    }
    cluster::MetadataCommand assign =
        metadataCommand(cluster::MetadataOperation::kAssignSlots, "assign-all");
    assign.slot_start = 0;
    assign.slot_end = cluster::kSlotCount - 1;
    assign.owner_group = 1;
    assign.expected_ownership_epoch = 0;
    assign.new_ownership_epoch = 1;
    expect(cluster.submitMetadata(assign), "shard 1 owns every slot");
    for (auto& node : cluster.nodes()) {
        node->shard(kSourceGroup).seedOwnership(slot, 1, 1);
    }
    cluster.publishTopology();

    // Step 0: real data on the source, including a TTL and several types.
    cluster.write(kSourceGroup, {"SET", slotKey("a"), "alpha"}, 1);
    cluster.write(kSourceGroup, {"SET", slotKey("b"), "beta"}, 1);
    cluster.write(kSourceGroup, {"SET", slotKey("gone"), "temporary"}, 1);
    cluster.write(kSourceGroup, {"RPUSH", slotKey("list"), "l1", "l2"}, 1);
    cluster.write(kSourceGroup, {"HSET", slotKey("hash"), "f", "v"}, 1);
    cluster.write(kSourceGroup, {"ZADD", slotKey("zset"), "1.5", "m"}, 1);
    cluster.write(kSourceGroup, {"SET", slotKey("ttl"), "expiring", "PX", "600000"}, 1);

    // Step 1: the control plane records the intent, with its own id and the
    // epoch on both sides of the move.
    cluster::MetadataCommand begin =
        metadataCommand(cluster::MetadataOperation::kBeginMigration, "begin");
    begin.slot_start = begin.slot_end = slot;
    begin.migration_id = kMigrationId;
    begin.source_group = 1;
    begin.target_group = 2;
    begin.expected_ownership_epoch = 1;
    begin.new_ownership_epoch = 2;
    expect(cluster.submitMetadata(begin), "migration intent commits");
    expect(cluster.submitMetadata(
               advanceTo(cluster::MigrationPhase::kPrepared, "prepared", 1)),
           "migration reaches Prepared");

    // Step 2: the source pins a base snapshot at one applied index; the target
    // opens a staging space that no client can see.
    expect(cluster.submitMigration(kSourceGroup,
                                   migrationOp(cluster::MigrationLogOp::kSourceBegin))
                   .status == cluster::MigrationApplyStatus::kApplied,
           "source pins the base snapshot");
    expect(cluster.submitMigration(kTargetGroup,
                                   migrationOp(cluster::MigrationLogOp::kTargetBegin))
                   .status == cluster::MigrationApplyStatus::kApplied,
           "target opens the staging space");
    expect(cluster.submitMetadata(
               advanceTo(cluster::MigrationPhase::kCopyingBase, "copying", 1)),
           "migration reaches CopyingBase");

    const consensus::Index base_index =
        cluster.leaderNode(kSourceGroup).shard(kSourceGroup).migration.find(kMigrationId)->base_index;
    expect(base_index > 0, "the base snapshot is pinned to a committed index");

    std::string cursor;
    bool complete = false;
    int chunks = 0;
    do {
        cluster::SlotMigrationCommand chunk =
            migrationOp(cluster::MigrationLogOp::kStageBase);
        chunk.records =
            cluster.leaderNode(kSourceGroup)
                .shard(kSourceGroup)
                .migration.baseChunkAfter(kMigrationId, cursor, 2, complete);
        chunk.base_complete = complete;
        if (!chunk.records.empty()) cursor = chunk.records.back().key;
        expect(cluster.submitMigration(kTargetGroup, chunk).status ==
                   cluster::MigrationApplyStatus::kApplied,
               "base chunk stages on the target");
        ++chunks;
    } while (!complete);
    expect(chunks > 1, "the base snapshot is shipped in several chunks");
    expect(cluster.leaderNode(kTargetGroup).shard(kTargetGroup).database.physicalKeyCount() == 0,
           "staged data stays out of the target keyspace");

    // Step 3: the source is still the only writer. Its post-snapshot writes turn
    // into deltas that the target applies through its own Raft.
    cluster.write(kSourceGroup, {"SET", slotKey("a"), "alpha-2"}, 1);
    cluster.write(kSourceGroup, {"DEL", slotKey("gone")}, 1);
    cluster.write(kSourceGroup, {"SADD", slotKey("set"), "s1", "s2"}, 1);
    cluster.write(kSourceGroup, {"RPUSH", slotKey("list"), "l3"}, 1);
    expect(cluster.submitMetadata(
               advanceTo(cluster::MigrationPhase::kCatchingUp, "catchup", 1)),
           "migration reaches CatchingUp");

    auto shipDeltas = [&cluster]() {
        for (int round = 0; round < 20; ++round) {
            ShardRuntime& target = cluster.leaderNode(kTargetGroup).shard(kTargetGroup);
            const std::uint64_t staged =
                target.migration.find(kMigrationId)->staged_sequence;
            cluster::SlotMigrationCommand batch =
                migrationOp(cluster::MigrationLogOp::kStageDelta);
            batch.records = cluster.leaderNode(kSourceGroup)
                                .shard(kSourceGroup)
                                .migration.deltasAfter(kMigrationId, staged, 3);
            if (batch.records.empty()) return;
            expect(cluster.submitMigration(kTargetGroup, batch).status ==
                       cluster::MigrationApplyStatus::kApplied,
                   "delta batch stages on the target");
        }
        expect(false, "delta shipping converges");
    };
    shipDeltas();

    // Replaying a batch the target already staged must be a no-op, because a
    // coordinator that dies mid-flight cannot know whether its last batch landed.
    cluster::SlotMigrationCommand replay =
        migrationOp(cluster::MigrationLogOp::kStageDelta);
    replay.records = cluster.leaderNode(kSourceGroup)
                         .shard(kSourceGroup)
                         .migration.deltasAfter(kMigrationId, 0, 3);
    expect(!replay.records.empty(), "the source retains its delta journal");
    expect(cluster.submitMigration(kTargetGroup, replay).status ==
               cluster::MigrationApplyStatus::kDuplicate,
           "re-shipping staged deltas is idempotent");

    // A write that lands after catch-up still becomes a delta.
    cluster.write(kSourceGroup, {"SET", slotKey("late"), "late-value"}, 1);

    // Step 4: the fence. From here the old epoch is dead everywhere.
    const cluster::MigrationApplyResult fenced = cluster.submitMigration(
        kSourceGroup, migrationOp(cluster::MigrationLogOp::kSourceFence));
    expect(fenced.status == cluster::MigrationApplyStatus::kApplied,
           "the source commits the fence");
    for (auto& node : cluster.nodes()) {
        expect(node->shard(kSourceGroup).ownership.get(slot).state ==
                   cluster::LocalSlotState::kSourceFenced,
               "every source replica records the fence");
    }
    const command::ApplyResult after_fence =
        cluster.write(kSourceGroup, {"SET", slotKey("a"), "must-not-apply"}, 1);
    expect(after_fence.status == command::ApplyStatus::kNoOpStaleEpoch,
           "a write committed after the fence applies as a no-op");
    std::string fenced_value;
    expect(cluster.leaderNode(kSourceGroup)
                   .shard(kSourceGroup)
                   .database.get(slotKey("a"), fenced_value) &&
               fenced_value == "alpha-2",
           "the fenced source keeps the value the target already has");

    // A deposed leader cannot sign the fence: the proof needs a read barrier
    // that a quorum acknowledged for the leader that is signing.
    const std::optional<consensus::Index> read_index =
        cluster.linearizableRead(kSourceGroup);
    expect(read_index.has_value(), "the source leader completes a linearizable read");
    cluster::MigrationCommitProof proof;
    std::string error;
    expect(cluster.leaderNode(kSourceGroup)
               .shard(kSourceGroup)
               .migration.buildCommitProof(kMigrationId, *read_index, proof, error),
           "the source leader builds a commit proof: " + error);
    expect(proof.read_index >= proof.fence_index && proof.fence_index >= base_index,
           "the proof covers the fence entry");
    const std::string encoded_proof = cluster::encodeCommitProof(proof, testMigrationKey());

    expect(cluster.submitMetadata(advanceTo(cluster::MigrationPhase::kSourceFenced,
                                            "fenced", 1, encoded_proof)),
           "the control plane records the fence with its proof");
    expect(!cluster.submitMetadata(
               advanceTo(cluster::MigrationPhase::kAborting, "too-late", 1)),
           "the control plane cannot abort a fenced migration");

    // Step 5: the target catches up to the fence, checks the digest, activates.
    shipDeltas();
    const cluster::LocalMigrationState* staged_state =
        cluster.leaderNode(kTargetGroup).shard(kTargetGroup).migration.find(kMigrationId);
    expect(staged_state->staged_sequence == proof.fence_sequence,
           "the target processed every change up to the fence index");

    cluster::SlotMigrationCommand activate =
        migrationOp(cluster::MigrationLogOp::kTargetActivate);
    activate.commit_proof = encoded_proof;
    expect(cluster.submitMigration(kTargetGroup, activate).status ==
               cluster::MigrationApplyStatus::kApplied,
           "the target activates after verifying the digest");
    for (auto& node : cluster.nodes()) {
        expect(node->shard(kTargetGroup).ownership.get(slot).state ==
                   cluster::LocalSlotState::kTargetActiveAsk,
               "every target replica activates in ASK-only mode");
    }

    const std::vector<cluster::SlotDataRecord> source_view = cluster::exportSlotRecords(
        cluster.leaderNode(kSourceGroup).shard(kSourceGroup).database, slot,
        proof.logical_time_ms);
    const std::vector<cluster::SlotDataRecord> target_view = cluster::exportSlotRecords(
        cluster.leaderNode(kTargetGroup).shard(kTargetGroup).database, slot,
        proof.logical_time_ms);
    expect(cluster::slotDigest(source_view, proof.logical_time_ms) ==
               cluster::slotDigest(target_view, proof.logical_time_ms),
           "the target keyspace matches the fenced source exactly");
    expect(cluster::slotDigest(target_view, proof.logical_time_ms) == proof.digest,
           "the promoted keyspace matches the digest in the proof");

    Database& moved = cluster.leaderNode(kTargetGroup).shard(kTargetGroup).database;
    std::string value;
    expect(moved.get(slotKey("a"), value) && value == "alpha-2",
           "post-snapshot updates arrived through the delta stream");
    expect(moved.get(slotKey("late"), value) && value == "late-value",
           "the last write before the fence arrived");
    expect(!moved.keyExists(slotKey("gone")), "deletes arrived as tombstones");
    expect(moved.lrange(slotKey("list"), 0, -1) ==
               std::vector<std::string>({"l1", "l2", "l3"}),
           "list state converged");
    expect(moved.smembers(slotKey("set")).size() == 2, "set state converged");

    StoredEntry source_ttl;
    StoredEntry target_ttl;
    expect(cluster.leaderNode(kSourceGroup)
                   .shard(kSourceGroup)
                   .database.exportKey(slotKey("ttl"), source_ttl) &&
               moved.exportKey(slotKey("ttl"), target_ttl) &&
               target_ttl.expire_at_ms == source_ttl.expire_at_ms &&
               target_ttl.expire_at_ms > 0,
           "the absolute expire deadline moved with the key");

    // The target proves, from its own committed log, that it holds exactly the
    // state the source fenced. Nothing downstream moves without this: not the
    // control plane's ASK window, and not the source's deletion of its copy.
    const std::optional<consensus::Index> target_read =
        cluster.linearizableRead(kTargetGroup);
    expect(target_read.has_value(), "the target leader completes a linearizable read");
    cluster::MigrationActivationProof activation;
    expect(cluster.leaderNode(kTargetGroup)
               .shard(kTargetGroup)
               .migration.buildActivationProof(kMigrationId, *target_read, activation,
                                               error),
           "the target builds an activation proof: " + error);
    expect(activation.digest == proof.digest,
           "the activation proof attests to the fenced state");
    const std::string encoded_activation =
        cluster::encodeActivationProof(activation, testMigrationKey());

    expect(!cluster.submitMetadata(advanceTo(cluster::MigrationPhase::kTargetActiveAsk,
                                             "unproven-active", 1, encoded_proof)),
           "the control plane will not open the window on the source's word alone");
    expect(cluster.submitMetadata(advanceTo(cluster::MigrationPhase::kTargetActiveAsk,
                                            "target-active", 1, encoded_proof,
                                            encoded_activation)),
           "the control plane opens the ASK window");
    cluster.publishTopology();

    Node& source_leader = cluster.leaderNode(kSourceGroup);
    cluster::ClientSession session;
    // The key is still physically present on the source here: it has been
    // fenced but not yet released. Routing must ignore that, because the copy
    // is frozen at the fence while the target is already taking writes.
    StoredEntry still_local;
    expect(source_leader.shard(kSourceGroup).database.exportKey(slotKey("a"),
                                                                still_local),
           "the fenced source still physically holds the key");
    const cluster::RouteDecision ask =
        source_leader.router.routeKeys({slotKey("a")}, session, 1, 1000);
    if (cluster.leaderNode(kTargetGroup).id == source_leader.id) {
        // One node leads both shards, so redirecting the client to itself would
        // only make it bounce; the router runs the request on the target shard.
        expect(ask.action == cluster::RouteAction::kLocal && ask.shard == 2 &&
                   ask.slot_epoch == 2 && ask.consumed_asking,
               "the source executes on the target shard it also leads");
    } else {
        expect(ask.action == cluster::RouteAction::kAsk && ask.shard == 2,
               "the source answers ASK for the slot it handed over");
    }

    // Only now does the source destroy what may still be the only other copy,
    // and only against evidence it can check against its own fence digest.
    cluster::SlotMigrationCommand unproven_release =
        migrationOp(cluster::MigrationLogOp::kSourceRelease);
    expect(cluster.submitMigration(kSourceGroup, unproven_release).status ==
               cluster::MigrationApplyStatus::kProofRejected,
           "the source will not delete the slot without an activation proof");

    cluster::MigrationActivationProof forged = activation;
    forged.digest = "00000000000000000000000000000000";
    cluster::SlotMigrationCommand forged_release =
        migrationOp(cluster::MigrationLogOp::kSourceRelease);
    forged_release.activation_proof =
        cluster::encodeActivationProof(forged, testMigrationKey());
    expect(cluster.submitMigration(kSourceGroup, forged_release).status ==
               cluster::MigrationApplyStatus::kDigestMismatch,
           "the source rejects an activation proof for a state it never fenced");

    cluster::SlotMigrationCommand unsigned_release =
        migrationOp(cluster::MigrationLogOp::kSourceRelease);
    unsigned_release.activation_proof =
        cluster::encodeActivationProof(activation, cluster::MigrationKey("outsider"));
    expect(cluster.submitMigration(kSourceGroup, unsigned_release).status ==
               cluster::MigrationApplyStatus::kProofRejected,
           "a proof signed outside the cluster is not a proof");

    cluster::SlotMigrationCommand release =
        migrationOp(cluster::MigrationLogOp::kSourceRelease);
    release.activation_proof = encoded_activation;
    expect(cluster.submitMigration(kSourceGroup, release).status ==
               cluster::MigrationApplyStatus::kApplied,
           "the fenced source releases the slot once the target has proven it");
    cluster.publishTopology();

    // Step 6: the control plane makes the target the owner at the new epoch.
    cluster::MetadataCommand commit =
        metadataCommand(cluster::MetadataOperation::kCommitMigration, "commit");
    commit.migration_id = kMigrationId;
    commit.expected_ownership_epoch = 1;
    commit.progress_proof = encoded_proof;
    commit.activation_proof = encoded_activation;
    expect(cluster.submitMetadata(commit), "the owner switches to the target");
    for (auto& node : cluster.nodes()) {
        const cluster::MetadataSlotRecord& record = node->metadata.metadata().slots[slot];
        expect(record.active_group == 2 && record.ownership_epoch == 2,
               "every node sees the new owner and epoch");
    }
    expect(cluster.submitMigration(kTargetGroup,
                                   migrationOp(cluster::MigrationLogOp::kTargetStable))
                   .status == cluster::MigrationApplyStatus::kApplied,
           "the target leaves ASK-only mode");
    cluster.publishTopology();

    const cluster::RouteDecision moved_route = cluster.leaderNode(kSourceGroup)
                                                   .router.routeKeys({slotKey("a")},
                                                                     session, 2, 1000);
    expect(moved_route.action == cluster::RouteAction::kMoved ||
               (moved_route.action == cluster::RouteAction::kLocal &&
                moved_route.shard == 2),
           "the slot now routes to the target shard");

    const command::ApplyResult target_write =
        cluster.write(kTargetGroup, {"SET", slotKey("a"), "owned-by-target"}, 2);
    expect(target_write.status == command::ApplyStatus::kApplied,
           "the new owner accepts writes at the new epoch without ASKING");

    // A delayed write carrying the old epoch must never overwrite the new owner.
    for (auto& node : cluster.nodes()) {
        expect(!node->shard(kTargetGroup).ownership.canAcceptWrite(slot, 2, 1, false),
               "a request stamped with the old epoch is refused by the new owner");
        expect(!node->shard(kSourceGroup).ownership.canAcceptWrite(slot, 1, 1, false),
               "the old owner never becomes writable again");
    }

    expect(cluster.submitMetadata(
               advanceTo(cluster::MigrationPhase::kCleanup, "cleanup", 2, encoded_proof,
                         encoded_activation)),
           "migration reaches Cleanup");
    cluster::MetadataCommand finish =
        metadataCommand(cluster::MetadataOperation::kFinishMigration, "finish");
    finish.migration_id = kMigrationId;
    expect(cluster.submitMetadata(finish), "migration record retires");
    expect(cluster.submitMigration(kSourceGroup, migrationOp(cluster::MigrationLogOp::kForget))
                   .status == cluster::MigrationApplyStatus::kApplied &&
               cluster.submitMigration(kTargetGroup,
                                       migrationOp(cluster::MigrationLogOp::kForget))
                       .status == cluster::MigrationApplyStatus::kApplied,
           "both shards drop the finished migration record");
    for (auto& node : cluster.nodes()) {
        expect(node->shard(kSourceGroup).database.exportKeys([slot](const std::string& key) {
                   return cluster::keyToSlot(key) == slot;
               }).empty(),
               "the source no longer stores the migrated slot");
    }
    expect(cluster.leaderNode(consensus::kMetadataGroupId).metadata.metadata().slots[slot].transition ==
               cluster::SlotTransition::kStable,
           "the slot is stable again under its new owner");
}

void testFenceSurvivesLeaderChange() {
    Cluster cluster;
    const cluster::SlotId slot = migratingSlot();
    cluster.elect(consensus::kMetadataGroupId);
    cluster.elect(kSourceGroup);

    for (auto& node : cluster.nodes()) {
        node->shard(kSourceGroup).seedOwnership(slot, 1, 1);
    }
    cluster.write(kSourceGroup, {"SET", slotKey("a"), "alpha"}, 1);
    cluster.submitMigration(kSourceGroup, migrationOp(cluster::MigrationLogOp::kSourceBegin));
    cluster.write(kSourceGroup, {"SET", slotKey("b"), "beta"}, 1);
    const cluster::MigrationApplyResult fenced = cluster.submitMigration(
        kSourceGroup, migrationOp(cluster::MigrationLogOp::kSourceFence));
    expect(fenced.status == cluster::MigrationApplyStatus::kApplied, "source fences");

    const consensus::NodeId old_leader = cluster.leaderNode(kSourceGroup).id;
    const cluster::LocalMigrationState* before =
        cluster.node(old_leader).shard(kSourceGroup).migration.find(kMigrationId);
    const consensus::Index fence_index = before->fence_index;
    const std::string fence_digest = before->fence_digest;

    cluster.transferLeadership(kSourceGroup);
    Node& new_leader = cluster.leaderNode(kSourceGroup);
    expect(new_leader.id != old_leader, "a different node leads the source shard");

    const cluster::LocalMigrationState* after =
        new_leader.shard(kSourceGroup).migration.find(kMigrationId);
    expect(after != nullptr && after->stage == cluster::LocalMigrationStage::kSourceFenced &&
               after->fence_index == fence_index && after->fence_digest == fence_digest,
           "the new leader inherits the committed fence, not a coordinator's memory");
    expect(new_leader.shard(kSourceGroup).ownership.get(slot).state ==
               cluster::LocalSlotState::kSourceFenced,
           "the new leader is still fenced");

    const command::ApplyResult attempted =
        cluster.write(kSourceGroup, {"SET", slotKey("a"), "new-leader-write"}, 1);
    expect(attempted.status == command::ApplyStatus::kNoOpStaleEpoch,
           "a new leader cannot reopen the fenced epoch");

    const std::optional<consensus::Index> read_index =
        cluster.linearizableRead(kSourceGroup);
    expect(read_index.has_value(), "the new leader completes a linearizable read");
    cluster::MigrationCommitProof proof;
    std::string error;
    expect(new_leader.shard(kSourceGroup)
               .migration.buildCommitProof(kMigrationId, *read_index, proof, error),
           "the new leader can still prove the fence");
    expect(proof.fence_index == fence_index && proof.digest == fence_digest,
           "the proof from the new leader describes the same fence");
}

} // namespace

int main() {
    testCodecsAndDigest();
    testFenceIsOneWay();
    testTargetRefusesUnsafeActivation();
    testDeltaJournalRequiresContiguity();
    testSnapshotResumesMidFlight();
    testJournalIgnoresVacuousDeletes();
    testReleaseLeavesTheEpochAdvanced();
    testOnlineMigration();
    testFenceSurvivesLeaderChange();
    std::cout << "Slot migration tests passed" << std::endl;
    return EXIT_SUCCESS;
}
