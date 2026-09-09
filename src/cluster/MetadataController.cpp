#include "mini_redis/cluster/MetadataController.hpp"

#include "mini_redis/cluster/MetadataWatcher.hpp"

namespace cluster {

consensus::ProposeResult MetadataController::propose(
    const MetadataCommand& command) {
    return raft_.group(consensus::kMetadataGroupId)
        .propose(encodeMetadataCommand(command));
}

MetadataApplyResult MetadataController::applyCommitted(
    const consensus::LogEntry& entry) {
    const std::uint64_t previous_revision = state_machine_.metadata().revision;
    MetadataApplyResult result = state_machine_.apply(entry);
    publishIfAdvanced(previous_revision);
    return result;
}

bool MetadataController::installSnapshot(const consensus::Snapshot& snapshot,
                                         std::string& error) {
    const std::uint64_t previous_revision = state_machine_.metadata().revision;
    if (!state_machine_.installSnapshot(snapshot.data, error)) {
        return false;
    }
    publishIfAdvanced(previous_revision);
    return true;
}

void MetadataController::publishIfAdvanced(std::uint64_t previous_revision) {
    if (watcher_ != nullptr &&
        state_machine_.metadata().revision > previous_revision) {
        watcher_->onCommitted(state_machine_.metadata());
    }
}

} // namespace cluster
