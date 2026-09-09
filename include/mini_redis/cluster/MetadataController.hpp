#pragma once

#include "mini_redis/cluster/MetadataStateMachine.hpp"
#include "mini_redis/consensus/MultiRaft.hpp"

namespace cluster {

class MetadataWatcher;

// Thin runtime adapter. The surrounding Ready driver remains responsible for
// persistence and transport; this class only binds group 0 to its state machine.
class MetadataController {
public:
    MetadataController(consensus::MultiRaft& raft, MetadataStateMachine& state_machine,
                       MetadataWatcher* watcher = nullptr)
        : raft_(raft), state_machine_(state_machine), watcher_(watcher) {}

    consensus::ProposeResult propose(const MetadataCommand& command);
    MetadataApplyResult applyCommitted(const consensus::LogEntry& entry);
    bool installSnapshot(const consensus::Snapshot& snapshot, std::string& error);

private:
    void publishIfAdvanced(std::uint64_t previous_revision);

    consensus::MultiRaft& raft_;
    MetadataStateMachine& state_machine_;
    MetadataWatcher* watcher_ = nullptr;
};

} // namespace cluster
