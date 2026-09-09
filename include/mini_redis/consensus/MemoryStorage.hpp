#pragma once

#include "mini_redis/consensus/RaftTypes.hpp"

namespace consensus {

// 测试与模拟器用的耐久层。生产环境由 persistence::PersistentStorage 替换，
// 驱动契约相同：先把 Ready 里的 hard state / 截断 / 条目 / snapshot 写稳，
// 再发消息、再 apply。
class MemoryStorage {
public:
    explicit MemoryStorage(Configuration bootstrap);

    void saveHardState(const HardState& hs);
    void truncateFrom(Index index);
    void append(const std::vector<LogEntry>& ents);
    void installSnapshot(const Snapshot& snapshot);
    void applyReady(const Ready& ready);

    HardState hardState() const { return hard_state_; }
    RaftRestore toRestore() const;
    const std::vector<LogEntry>& entries() const { return entries_; }
    Index lastIndex() const;
    Configuration latestConf() const { return conf_; }
    const Snapshot& snapshot() const { return snapshot_; }

private:
    Configuration lastConfigFromLog() const;

    HardState hard_state_;
    Configuration conf_;
    Snapshot snapshot_;
    std::vector<LogEntry> entries_;
};

} // namespace consensus
