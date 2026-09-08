#include "mini_redis/consensus/MultiRaft.hpp"

#include <stdexcept>
#include <utility>

namespace consensus {

MultiRaft::MultiRaft(NodeId self, RaftOptions defaults)
    : self_(std::move(self)), defaults_(std::move(defaults)) {}

void MultiRaft::createGroup(GroupId id, const Configuration& conf) {
    RaftRestore restore;
    restore.conf = normalizeConf(conf);
    createGroup(id, std::move(restore), defaults_);
}

void MultiRaft::createGroup(GroupId id, RaftRestore restore, RaftOptions options) {
    if (groups_.count(id) != 0) {
        throw std::invalid_argument("raft group already exists");
    }
    groups_[id] = std::make_unique<RaftCore>(self_, id, std::move(restore), std::move(options));
}

void MultiRaft::removeGroup(GroupId id) {
    groups_.erase(id);
}

bool MultiRaft::hasGroup(GroupId id) const {
    return groups_.count(id) != 0;
}

RaftCore& MultiRaft::group(GroupId id) {
    const auto it = groups_.find(id);
    if (it == groups_.end()) {
        throw std::out_of_range("unknown raft group");
    }
    return *it->second;
}

const RaftCore& MultiRaft::group(GroupId id) const {
    const auto it = groups_.find(id);
    if (it == groups_.end()) {
        throw std::out_of_range("unknown raft group");
    }
    return *it->second;
}

std::vector<GroupId> MultiRaft::groupIds() const {
    std::vector<GroupId> ids;
    ids.reserve(groups_.size());
    for (const auto& item : groups_) {
        ids.push_back(item.first);
    }
    return ids;
}

void MultiRaft::tick(std::int64_t now_ms) {
    for (auto& item : groups_) {
        item.second->tick(now_ms);
    }
}

void MultiRaft::step(const Message& msg) {
    const auto it = groups_.find(msg.group);
    if (it == groups_.end()) {
        return;
    }
    it->second->step(msg);
}

Configuration MultiRaft::threeVoters(const NodeId& a, const NodeId& b, const NodeId& c) {
    Configuration conf;
    conf.incoming.voters = {a, b, c};
    return normalizeConf(std::move(conf));
}

} // namespace consensus
