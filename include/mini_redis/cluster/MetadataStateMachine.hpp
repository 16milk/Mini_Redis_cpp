#pragma once

#include "mini_redis/cluster/MetadataCommand.hpp"
#include "mini_redis/consensus/RaftTypes.hpp"

#include <map>
#include <string>

namespace cluster {

enum class MetadataApplyStatus : std::uint8_t {
    kApplied = 1,
    kInvalidCommand = 2,
    kCasMismatch = 3,
    kRequestConflict = 4,
    kNotFound = 5,
    kIllegalTransition = 6,
    kStaleEpoch = 7,
};

struct MetadataApplyResult {
    MetadataApplyStatus status = MetadataApplyStatus::kInvalidCommand;
    std::uint64_t revision = 0;
    bool duplicate = false;
    std::string error;

    bool applied() const { return status == MetadataApplyStatus::kApplied; }
};

class MetadataStateMachine {
public:
    MetadataApplyResult apply(const consensus::LogEntry& entry);
    MetadataApplyResult apply(const std::string& payload);
    MetadataApplyResult apply(const MetadataCommand& command);

    const ClusterMetadata& metadata() const { return metadata_; }

    // Snapshot bytes include request deduplication outcomes so a retry after
    // compaction/restart remains idempotent.
    std::string snapshotBytes() const;
    bool installSnapshot(const std::string& bytes, std::string& error);

private:
    struct DedupRecord {
        std::string encoded_command;
        MetadataApplyResult result;
    };

    MetadataApplyResult applyNew(const MetadataCommand& command);
    MetadataApplyResult reject(MetadataApplyStatus status, std::string error) const;
    MetadataApplyResult remember(const MetadataCommand& command,
                                 MetadataApplyResult result);

    ClusterMetadata metadata_;
    std::map<std::string, DedupRecord> dedup_;
};

} // namespace cluster
