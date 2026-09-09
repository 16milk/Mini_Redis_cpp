#include "mini_redis/cluster/MetadataStateMachine.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

cluster::MetadataCommand base(cluster::MetadataOperation operation,
                              std::string request_id,
                              std::uint64_t revision) {
    cluster::MetadataCommand command;
    command.operation = operation;
    command.request_id = std::move(request_id);
    command.expected_revision = revision;
    return command;
}

cluster::MetadataNodeRecord node(const std::string& id, std::uint16_t port) {
    cluster::MetadataNodeRecord record;
    record.id = id;
    record.client = {"127.0.0.1", port};
    record.internal = {"127.0.0.1", static_cast<std::uint16_t>(port + 1000)};
    record.admin = {"127.0.0.1", static_cast<std::uint16_t>(port + 2000)};
    record.failure_domain = id;
    return record;
}

cluster::MetadataApplyResult apply(cluster::MetadataStateMachine& machine,
                                   const cluster::MetadataCommand& command) {
    const cluster::MetadataApplyResult result = machine.apply(command);
    expect(result.applied(), "metadata command should apply: " + result.error);
    return result;
}

} // namespace

int main() {
    cluster::MetadataStateMachine machine;

    cluster::MetadataCommand initialize =
        base(cluster::MetadataOperation::kInitializeCluster, "init", 0);
    initialize.cluster_id = "cluster-a";
    expect(apply(machine, initialize).revision == 1, "initialize revision");
    const cluster::MetadataApplyResult duplicate = machine.apply(initialize);
    expect(duplicate.applied() && duplicate.duplicate && duplicate.revision == 1,
           "same request id and payload is idempotent");

    cluster::MetadataCommand conflicting = initialize;
    conflicting.cluster_id = "cluster-b";
    expect(machine.apply(conflicting).status ==
               cluster::MetadataApplyStatus::kRequestConflict,
           "same request id with another payload is rejected");

    for (int index = 1; index <= 3; ++index) {
        cluster::MetadataCommand add =
            base(cluster::MetadataOperation::kUpsertNode,
                 "node-" + std::to_string(index), machine.metadata().revision);
        add.node = node("n" + std::to_string(index),
                        static_cast<std::uint16_t>(7000 + index));
        apply(machine, add);
    }

    cluster::MetadataCommand group1 =
        base(cluster::MetadataOperation::kUpsertGroup, "group-1",
             machine.metadata().revision);
    group1.group.id = 1;
    group1.group.voters = {"n1", "n2", "n3"};
    group1.group.desired_placement = group1.group.voters;
    apply(machine, group1);

    cluster::MetadataCommand group2 =
        base(cluster::MetadataOperation::kUpsertGroup, "group-2",
             machine.metadata().revision);
    group2.group.id = 2;
    group2.group.voters = {"n1", "n2", "n3"};
    apply(machine, group2);

    cluster::MetadataCommand assign =
        base(cluster::MetadataOperation::kAssignSlots, "assign-42",
             machine.metadata().revision);
    assign.slot_start = assign.slot_end = 42;
    assign.owner_group = 1;
    assign.expected_ownership_epoch = 0;
    assign.new_ownership_epoch = 1;
    apply(machine, assign);

    cluster::MetadataCommand unsafe_reassign =
        base(cluster::MetadataOperation::kAssignSlots, "unsafe-reassign",
             machine.metadata().revision);
    unsafe_reassign.slot_start = unsafe_reassign.slot_end = 42;
    unsafe_reassign.owner_group = 2;
    unsafe_reassign.expected_ownership_epoch = 1;
    unsafe_reassign.new_ownership_epoch = 2;
    expect(machine.apply(unsafe_reassign).status ==
               cluster::MetadataApplyStatus::kInvalidCommand,
           "metadata cannot create ownership without migration proofs");

    cluster::MetadataCommand stale =
        base(cluster::MetadataOperation::kAssignSlots, "stale-revision",
             machine.metadata().revision - 1);
    stale.slot_start = stale.slot_end = 42;
    stale.owner_group = 1;
    stale.expected_ownership_epoch = 1;
    stale.new_ownership_epoch = 2;
    expect(machine.apply(stale).status == cluster::MetadataApplyStatus::kCasMismatch,
           "stale metadata revision is rejected");

    cluster::MetadataCommand begin =
        base(cluster::MetadataOperation::kBeginMigration, "migration-begin",
             machine.metadata().revision);
    begin.slot_start = begin.slot_end = 42;
    begin.migration_id = "migration-42";
    begin.source_group = 1;
    begin.target_group = 2;
    begin.expected_ownership_epoch = 1;
    begin.new_ownership_epoch = 2;
    apply(machine, begin);

    const cluster::MigrationPhase phases[] = {
        cluster::MigrationPhase::kPrepared,
        cluster::MigrationPhase::kCopyingBase,
        cluster::MigrationPhase::kCatchingUp,
        cluster::MigrationPhase::kSourceFenced,
    };
    int phase_request = 0;
    for (cluster::MigrationPhase phase : phases) {
        cluster::MetadataCommand advance =
            base(cluster::MetadataOperation::kAdvanceMigration,
                 "phase-" + std::to_string(++phase_request),
                 machine.metadata().revision);
        advance.migration_id = "migration-42";
        advance.expected_ownership_epoch = 1;
        advance.migration_phase = phase;
        advance.progress_proof = "proof-" + std::to_string(phase_request);
        apply(machine, advance);
    }

    cluster::MetadataCommand rollback =
        base(cluster::MetadataOperation::kAdvanceMigration, "unsafe-rollback",
             machine.metadata().revision);
    rollback.migration_id = "migration-42";
    rollback.expected_ownership_epoch = 1;
    rollback.migration_phase = cluster::MigrationPhase::kAborting;
    expect(machine.apply(rollback).status ==
               cluster::MetadataApplyStatus::kIllegalTransition,
           "source fence is a roll-forward boundary");

    cluster::MetadataCommand activate =
        base(cluster::MetadataOperation::kAdvanceMigration, "target-active",
             machine.metadata().revision);
    activate.migration_id = "migration-42";
    activate.expected_ownership_epoch = 1;
    activate.migration_phase = cluster::MigrationPhase::kTargetActiveAsk;
    apply(machine, activate);

    cluster::MetadataCommand commit =
        base(cluster::MetadataOperation::kCommitMigration, "metadata-commit",
             machine.metadata().revision);
    commit.migration_id = "migration-42";
    commit.expected_ownership_epoch = 1;
    commit.progress_proof = "verified-proof-tuple";
    apply(machine, commit);
    expect(machine.metadata().slots[42].active_group == 2 &&
               machine.metadata().slots[42].ownership_epoch == 2,
           "owner and epoch change atomically at metadata commit");

    cluster::MetadataCommand cleanup =
        base(cluster::MetadataOperation::kAdvanceMigration, "cleanup",
             machine.metadata().revision);
    cleanup.migration_id = "migration-42";
    cleanup.expected_ownership_epoch = 2;
    cleanup.migration_phase = cluster::MigrationPhase::kCleanup;
    apply(machine, cleanup);

    cluster::MetadataCommand finish =
        base(cluster::MetadataOperation::kFinishMigration, "finish",
             machine.metadata().revision);
    finish.migration_id = "migration-42";
    const cluster::MetadataApplyResult finish_result = apply(machine, finish);
    expect(machine.metadata().migrations.empty() &&
               machine.metadata().slots[42].transition ==
                   cluster::SlotTransition::kStable,
           "cleanup returns the slot to stable");

    const std::string snapshot = machine.snapshotBytes();
    cluster::MetadataStateMachine restored;
    std::string error;
    expect(restored.installSnapshot(snapshot, error), "metadata snapshot restores");
    expect(restored.metadata().revision == machine.metadata().revision &&
               restored.metadata().slots[42].active_group == 2,
           "metadata snapshot preserves topology");
    const cluster::MetadataApplyResult restored_duplicate = restored.apply(finish);
    expect(restored_duplicate.duplicate &&
               restored_duplicate.revision == finish_result.revision,
           "metadata snapshot preserves request deduplication");
    expect(!restored.installSnapshot(snapshot + "x", error),
           "metadata snapshot rejects trailing bytes");

    cluster::MetadataCommand decoded;
    expect(cluster::decodeMetadataCommand(
               cluster::encodeMetadataCommand(assign), decoded, error) &&
               decoded.request_id == assign.request_id,
           "metadata command codec round trip");
    expect(!cluster::decodeMetadataCommand("broken", decoded, error),
           "metadata command codec rejects corrupt input");

    std::cout << "Metadata state machine tests passed" << std::endl;
    return EXIT_SUCCESS;
}
