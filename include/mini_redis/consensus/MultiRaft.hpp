#pragma once

#include "mini_redis/consensus/RaftCore.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace consensus {

// 一个进程里的 Multi-Raft 宿主：每个逻辑分片一个独立 Group，
// 彼此有独立的 term、日志、选举计时器和成员配置。
class MultiRaft {
public:
    explicit MultiRaft(NodeId self, RaftOptions defaults = {});

    const NodeId& selfId() const { return self_; }

    void createGroup(GroupId id, const Configuration& conf);
    void createGroup(GroupId id, RaftRestore restore, RaftOptions options);
    void removeGroup(GroupId id);

    bool hasGroup(GroupId id) const;
    RaftCore& group(GroupId id);
    const RaftCore& group(GroupId id) const;
    std::vector<GroupId> groupIds() const;
    std::size_t groupCount() const { return groups_.size(); }

    void tick(std::int64_t now_ms);
    void step(const Message& msg);

    static Configuration threeVoters(const NodeId& a, const NodeId& b, const NodeId& c);

private:
    NodeId self_;
    RaftOptions defaults_;
    std::unordered_map<GroupId, std::unique_ptr<RaftCore>> groups_;
};

} // namespace consensus
