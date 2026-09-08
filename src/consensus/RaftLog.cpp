#include "mini_redis/consensus/RaftLog.hpp"

#include <algorithm>

namespace consensus {

RaftLog::RaftLog(const RaftRestore& restore)
    : snapshot_index_(restore.snapshot_index), snapshot_term_(restore.snapshot_term),
      entries_(restore.entries) {}

Term RaftLog::lastTerm() const {
    if (entries_.empty()) {
        return snapshot_term_;
    }
    return entries_.back().term;
}

std::optional<Term> RaftLog::term(Index index) const {
    if (index == 0) {
        return 0;
    }
    if (index == snapshot_index_) {
        return snapshot_term_;
    }
    if (index < firstIndex() || index > lastIndex()) {
        return std::nullopt;
    }
    return entries_[offset(index)].term;
}

bool RaftLog::has(Index index) const {
    return index >= firstIndex() && index <= lastIndex();
}

const LogEntry* RaftLog::entry(Index index) const {
    if (!has(index)) {
        return nullptr;
    }
    return &entries_[offset(index)];
}

std::vector<LogEntry> RaftLog::slice(Index from_inclusive, Index to_exclusive) const {
    std::vector<LogEntry> out;
    if (from_inclusive >= to_exclusive) {
        return out;
    }
    const Index from = std::max(from_inclusive, firstIndex());
    const Index to = std::min(to_exclusive, lastIndex() + 1);
    if (from >= to) {
        return out;
    }
    out.assign(entries_.begin() + static_cast<std::ptrdiff_t>(offset(from)),
               entries_.begin() + static_cast<std::ptrdiff_t>(offset(to)));
    return out;
}

void RaftLog::append(const std::vector<LogEntry>& ents) {
    entries_.insert(entries_.end(), ents.begin(), ents.end());
}

bool RaftLog::maybeAppend(Index prev_index, Term prev_term, const std::vector<LogEntry>& ents,
                          Index& truncated_from) {
    truncated_from = 0;
    const std::optional<Term> local_prev = term(prev_index);
    if (!local_prev.has_value() || *local_prev != prev_term) {
        return false;
    }
    if (ents.empty()) {
        return true;
    }

    Index next = prev_index + 1;
    std::size_t skip = 0;
    for (; skip < ents.size(); ++skip, ++next) {
        const std::optional<Term> existing = term(next);
        if (!existing.has_value()) {
            break;
        }
        if (*existing != ents[skip].term) {
            truncated_from = next;
            entries_.resize(offset(next));
            break;
        }
    }
    if (skip < ents.size()) {
        entries_.insert(entries_.end(), ents.begin() + static_cast<std::ptrdiff_t>(skip),
                        ents.end());
    }
    return true;
}

void RaftLog::compact(Index index, Term compact_term) {
    if (index <= snapshot_index_) {
        return;
    }
    if (index > lastIndex()) {
        entries_.clear();
        snapshot_index_ = index;
        snapshot_term_ = compact_term;
        return;
    }
    entries_.erase(entries_.begin(),
                   entries_.begin() + static_cast<std::ptrdiff_t>(offset(index) + 1));
    snapshot_index_ = index;
    snapshot_term_ = compact_term;
}

void RaftLog::installSnapshot(const Snapshot& snapshot) {
    entries_.clear();
    snapshot_index_ = snapshot.last_included_index;
    snapshot_term_ = snapshot.last_included_term;
}

std::size_t RaftLog::offset(Index index) const {
    return static_cast<std::size_t>(index - firstIndex());
}

} // namespace consensus
