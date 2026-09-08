#include "mini_redis/consensus/RaftTypes.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace consensus {
namespace {

std::vector<NodeId> uniqueSorted(std::vector<NodeId> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool contains(const std::vector<NodeId>& ids, const NodeId& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

Membership normalizeMembership(Membership membership) {
    membership.voters = uniqueSorted(std::move(membership.voters));
    membership.learners = uniqueSorted(std::move(membership.learners));
    std::vector<NodeId> learners;
    learners.reserve(membership.learners.size());
    for (const NodeId& id : membership.learners) {
        if (!contains(membership.voters, id)) {
            learners.push_back(id);
        }
    }
    membership.learners = std::move(learners);
    return membership;
}

} // namespace

bool isVoter(const Configuration& conf, const NodeId& id) {
    return contains(conf.incoming.voters, id) || contains(conf.outgoing.voters, id);
}

bool isLearner(const Configuration& conf, const NodeId& id) {
    if (isVoter(conf, id)) {
        return false;
    }
    return contains(conf.incoming.learners, id) || contains(conf.outgoing.learners, id);
}

bool isMember(const Configuration& conf, const NodeId& id) {
    return isVoter(conf, id) || isLearner(conf, id);
}

std::vector<NodeId> allVoters(const Configuration& conf) {
    std::vector<NodeId> voters = conf.incoming.voters;
    voters.insert(voters.end(), conf.outgoing.voters.begin(), conf.outgoing.voters.end());
    return uniqueSorted(std::move(voters));
}

std::vector<NodeId> allPeers(const Configuration& conf, const NodeId& self) {
    std::set<NodeId> ids;
    const auto add = [&ids, &self](const std::vector<NodeId>& group) {
        for (const NodeId& id : group) {
            if (id != self) {
                ids.insert(id);
            }
        }
    };
    add(conf.incoming.voters);
    add(conf.incoming.learners);
    add(conf.outgoing.voters);
    add(conf.outgoing.learners);
    return std::vector<NodeId>(ids.begin(), ids.end());
}

Configuration normalizeConf(Configuration conf) {
    conf.incoming = normalizeMembership(std::move(conf.incoming));
    conf.outgoing = normalizeMembership(std::move(conf.outgoing));
    if (conf.outgoing.voters.empty()) {
        conf.outgoing = Membership{};
    }
    return conf;
}

Configuration jointConf(const Membership& old_membership, const Membership& new_membership) {
    Configuration conf;
    conf.outgoing = normalizeMembership(old_membership);
    conf.incoming = normalizeMembership(new_membership);
    return normalizeConf(std::move(conf));
}

Configuration leaveJoint(const Configuration& conf) {
    Configuration next;
    next.incoming = conf.incoming;
    return normalizeConf(std::move(next));
}

bool hasMajority(const std::vector<NodeId>& voters, const std::vector<NodeId>& acked) {
    if (voters.empty()) {
        return false;
    }
    std::size_t count = 0;
    for (const NodeId& voter : voters) {
        if (contains(acked, voter)) {
            ++count;
        }
    }
    return count * 2 > voters.size();
}

bool quorumAcked(const Configuration& conf, const std::vector<NodeId>& acked) {
    if (!hasMajority(conf.incoming.voters, acked)) {
        return false;
    }
    if (conf.joint() && !hasMajority(conf.outgoing.voters, acked)) {
        return false;
    }
    return true;
}

} // namespace consensus
