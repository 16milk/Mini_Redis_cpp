#pragma once

#include "mini_redis/consensus/RaftTypes.hpp"

#include <optional>

namespace consensus {

// 内存日志。snapshot_index 之前的条目已被压缩；index 0 是哨兵，term 为 0。
class RaftLog {
public:
    RaftLog() = default;
    explicit RaftLog(const RaftRestore& restore);

    Index snapshotIndex() const { return snapshot_index_; }
    Term snapshotTerm() const { return snapshot_term_; }
    Index firstIndex() const { return snapshot_index_ + 1; }
    Index lastIndex() const { return snapshot_index_ + static_cast<Index>(entries_.size()); }
    Term lastTerm() const;

    // index == 0 时返回 0。被压缩或不存在时返回 nullopt。
    std::optional<Term> term(Index index) const;
    bool has(Index index) const;

    const LogEntry* entry(Index index) const;
    std::vector<LogEntry> slice(Index from_inclusive, Index to_exclusive) const;

    // Leader 在末尾追加连续条目。
    void append(const std::vector<LogEntry>& ents);

    // Follower 按 prevLog 对齐后追加。冲突条目从第一个不同 term 处截断。
    bool maybeAppend(Index prev_index, Term prev_term, const std::vector<LogEntry>& ents,
                     Index& truncated_from);

    void compact(Index index, Term term);
    void installSnapshot(const Snapshot& snapshot);

    const std::vector<LogEntry>& entries() const { return entries_; }

private:
    std::size_t offset(Index index) const;

    Index snapshot_index_ = 0;
    Term snapshot_term_ = 0;
    std::vector<LogEntry> entries_;
};

} // namespace consensus
