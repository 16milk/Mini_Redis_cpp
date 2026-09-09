#pragma once

#include "mini_redis/consensus/RaftTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace persistence {

struct IoStats {
    std::uint64_t wal_fsyncs = 0;
    std::uint64_t atomic_writes = 0;
    std::uint64_t records_appended = 0;
};

struct OpenOptions {
    std::string data_dir;
    std::string cluster_id;
    consensus::NodeId node_id;
    consensus::GroupId group_id = 0;
    consensus::Configuration bootstrap;
    std::size_t segment_bytes = 64U * 1024U * 1024U;
};

// Durable Raft log for one group. The contract matches MemoryStorage: the Ready
// driver must finish applyReady() before sending messages or applying committed
// entries, so a Follower's success response is only sent after the required
// records are fsync'd.
//
// On-disk layout (under data_dir):
//   identity
//   groups/<group_id>/hard-state
//   groups/<group_id>/CURRENT
//   groups/<group_id>/wal/segment-*.wal
//   groups/<group_id>/snapshots/<index>-<term>/{manifest,payload}
//
// RDB files are never consulted here. Redis dump format is import/export only.
class PersistentStorage {
public:
    static std::unique_ptr<PersistentStorage> open(const OpenOptions& options,
                                                   std::string& error);
    ~PersistentStorage();
    PersistentStorage(const PersistentStorage&) = delete;
    PersistentStorage& operator=(const PersistentStorage&) = delete;

    bool saveHardState(const consensus::HardState& hs);
    bool truncateFrom(consensus::Index index);
    bool append(const std::vector<consensus::LogEntry>& ents);
    bool installSnapshot(const consensus::Snapshot& snapshot);
    bool applyReady(const consensus::Ready& ready);

    // Pin WAL after `index` so snapshot compaction cannot recycle it while a
    // migration still needs the suffix. Recycle runs only after a snapshot is
    // installed and durable.
    void pinWalAfter(const std::string& pin_id, consensus::Index index);
    void unpinWal(const std::string& pin_id);

    consensus::HardState hardState() const;
    consensus::RaftRestore toRestore() const;
    const consensus::Snapshot& snapshot() const;
    const std::vector<consensus::LogEntry>& entries() const;
    std::vector<consensus::LogEntry> committedReplay() const;
    consensus::Configuration latestConf() const;
    consensus::Index lastIndex() const;
    consensus::Index recycleThroughIndex() const;

    const IoStats& stats() const;
    const std::string& lastError() const;
    std::string groupDir() const;
    std::vector<std::string> walSegmentPaths() const;
    std::size_t walSegmentCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PersistentStorage(std::unique_ptr<Impl> impl);
};

}  // namespace persistence
