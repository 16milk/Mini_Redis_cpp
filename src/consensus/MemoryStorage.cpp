#include "mini_redis/consensus/MemoryStorage.hpp"

#include <algorithm>

namespace consensus {

MemoryStorage::MemoryStorage(Configuration bootstrap) : conf_(normalizeConf(std::move(bootstrap))) {}

void MemoryStorage::saveHardState(const HardState& hs) {
    hard_state_ = hs;
}

void MemoryStorage::truncateFrom(Index index) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [index](const LogEntry& entry) { return entry.index >= index; }),
                   entries_.end());
    conf_ = lastConfigFromLog();
}

void MemoryStorage::append(const std::vector<LogEntry>& ents) {
    for (const LogEntry& entry : ents) {
        if (!entries_.empty() && entry.index <= entries_.back().index) {
            truncateFrom(entry.index);
        }
        entries_.push_back(entry);
        if (entry.type == EntryType::kConfig) {
            conf_ = normalizeConf(entry.config);
        }
    }
}

void MemoryStorage::installSnapshot(const Snapshot& snapshot) {
    snapshot_ = snapshot;
    conf_ = normalizeConf(snapshot.conf);
    truncateFrom(snapshot.last_included_index + 1);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&snapshot](const LogEntry& entry) {
                                      return entry.index <= snapshot.last_included_index;
                                  }),
                   entries_.end());
    if (hard_state_.commit_index < snapshot.last_included_index) {
        hard_state_.commit_index = snapshot.last_included_index;
    }
}

void MemoryStorage::applyReady(const Ready& ready) {
    if (ready.snapshot.has_value()) {
        installSnapshot(*ready.snapshot);
    }
    if (ready.truncate_from.has_value()) {
        truncateFrom(*ready.truncate_from);
    }
    if (!ready.entries.empty()) {
        append(ready.entries);
    }
    if (ready.hard_state.has_value()) {
        saveHardState(*ready.hard_state);
    }
}

Index MemoryStorage::lastIndex() const {
    if (!entries_.empty()) {
        return entries_.back().index;
    }
    return snapshot_.last_included_index;
}

RaftRestore MemoryStorage::toRestore() const {
    RaftRestore restore;
    restore.hard_state = hard_state_;
    restore.conf = conf_;
    restore.snapshot_conf =
        snapshot_.last_included_index == 0 ? Configuration{} : snapshot_.conf;
    restore.snapshot_index = snapshot_.last_included_index;
    restore.snapshot_term = snapshot_.last_included_term;
    restore.snapshot_data = snapshot_.data;
    restore.entries = entries_;
    return restore;
}

Configuration MemoryStorage::lastConfigFromLog() const {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->type == EntryType::kConfig) {
            return normalizeConf(it->config);
        }
    }
    return snapshot_.last_included_index == 0 ? conf_ : normalizeConf(snapshot_.conf);
}

} // namespace consensus
