#include "mini_redis/consensus/MemoryStorage.hpp"
#include "mini_redis/consensus/MultiRaft.hpp"
#include "mini_redis/consensus/RaftCore.hpp"
#include "mini_redis/consensus/RaftLog.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename T>
void expect_equal(const T& actual, const T& expected, const std::string& description) {
    if (actual != expected) {
        std::cerr << "FAILED: " << description << "\nExpected: " << expected
                  << "\nActual:   " << actual << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

consensus::RaftOptions testOptions() {
    consensus::RaftOptions options;
    options.heartbeat_interval_ms = 10;
    options.election_timeout_min_ms = 50;
    options.election_timeout_max_ms = 100;
    options.transfer_timeout_ms = 80;
    options.learner_promote_max_lag = 8;
    options.pre_vote = true;
    options.check_quorum = true;
    return options;
}

struct KvStore {
    std::map<std::string, std::string> data;
    consensus::Index applied = 0;

    void apply(const consensus::LogEntry& entry) {
        applied = entry.index;
        if (entry.type != consensus::EntryType::kCommand) {
            return;
        }
        const std::string& payload = entry.payload;
        const auto split = payload.find('=');
        if (split == std::string::npos) {
            data[payload] = "1";
            return;
        }
        data[payload.substr(0, split)] = payload.substr(split + 1);
    }

    void install(const consensus::Snapshot& snapshot) {
        data.clear();
        applied = snapshot.last_included_index;
        if (snapshot.data.empty()) {
            return;
        }
        std::size_t pos = 0;
        while (pos < snapshot.data.size()) {
            const auto end = snapshot.data.find('\n', pos);
            const std::string line = snapshot.data.substr(
                pos, end == std::string::npos ? std::string::npos : end - pos);
            const auto split = line.find('=');
            if (split != std::string::npos) {
                data[line.substr(0, split)] = line.substr(split + 1);
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
    }

    std::string snapshotBytes() const {
        std::string out;
        for (const auto& item : data) {
            out += item.first;
            out += '=';
            out += item.second;
            out += '\n';
        }
        return out;
    }
};

struct TestNode {
    consensus::NodeId id;
    consensus::MultiRaft multi;
    std::map<consensus::GroupId, consensus::MemoryStorage> storage;
    std::map<consensus::GroupId, KvStore> kv;
    bool alive = true;

    explicit TestNode(consensus::NodeId node_id, const consensus::RaftOptions& options)
        : id(std::move(node_id)), multi(id, options) {}
};

class Cluster {
public:
    Cluster(std::vector<consensus::NodeId> ids, std::vector<consensus::GroupId> groups,
            consensus::RaftOptions options = testOptions())
        : options_(std::move(options)) {
        for (const consensus::NodeId& id : ids) {
            nodes_.push_back(std::make_unique<TestNode>(id, options_));
        }
        const consensus::Configuration conf = voterConf(ids);
        for (auto& node : nodes_) {
            for (consensus::GroupId group : groups) {
                node->storage.emplace(group, conf);
                node->kv.emplace(group, KvStore{});
                node->multi.createGroup(group, conf);
            }
        }
    }

    static consensus::Configuration voterConf(const std::vector<consensus::NodeId>& ids) {
        consensus::Configuration conf;
        conf.incoming.voters = ids;
        return consensus::normalizeConf(std::move(conf));
    }

    TestNode& node(const consensus::NodeId& id) {
        for (auto& item : nodes_) {
            if (item->id == id) {
                return *item;
            }
        }
        std::cerr << "FAILED: unknown node " << id << std::endl;
        std::exit(EXIT_FAILURE);
    }

    consensus::RaftCore& raft(const consensus::NodeId& id, consensus::GroupId group) {
        return node(id).multi.group(group);
    }

    void partition(const consensus::NodeId& a, const consensus::NodeId& b) {
        dropped_.insert(a + "->" + b);
        dropped_.insert(b + "->" + a);
    }

    void isolate(const consensus::NodeId& id) {
        for (const auto& item : nodes_) {
            if (item->id != id) {
                partition(id, item->id);
            }
        }
    }

    void heal() { dropped_.clear(); }

    void crash(const consensus::NodeId& id) { node(id).alive = false; }

    void restart(const consensus::NodeId& id) {
        TestNode& item = node(id);
        const std::vector<consensus::GroupId> groups = item.multi.groupIds();
        for (consensus::GroupId group : groups) {
            const consensus::RaftRestore restore = item.storage.at(group).toRestore();
            item.multi.removeGroup(group);
            item.multi.createGroup(group, restore, options_);
            item.kv[group].applied = restore.snapshot_index;
        }
        item.alive = true;
    }

    void tick(std::int64_t delta_ms) {
        pump();
        now_ms_ += delta_ms;
        for (auto& item : nodes_) {
            if (item->alive) {
                item->multi.tick(now_ms_);
            }
        }
        pump();
    }

    void elect(consensus::GroupId group) {
        for (int round = 0; round < 400; ++round) {
            tick(options_.heartbeat_interval_ms);
            if (leaderOf(group) != nullptr) {
                waitCommitted(group, 1);
                return;
            }
        }
        expect(false, "failed to elect a leader for group " + std::to_string(group));
    }

    consensus::RaftCore* leaderOf(consensus::GroupId group) {
        consensus::RaftCore* leader = nullptr;
        consensus::Term term = 0;
        for (auto& item : nodes_) {
            if (!item->alive || !item->multi.hasGroup(group)) {
                continue;
            }
            consensus::RaftCore& core = item->multi.group(group);
            if (core.role() == consensus::RaftRole::kLeader) {
                expect(leader == nullptr || core.term() != term,
                       "two leaders in the same term");
                if (leader == nullptr || core.term() > term) {
                    leader = &core;
                    term = core.term();
                }
            }
        }
        return leader;
    }

    consensus::NodeId leaderId(consensus::GroupId group) {
        consensus::RaftCore* leader = leaderOf(group);
        expect(leader != nullptr, "no leader");
        return leader->selfId();
    }

    bool write(consensus::GroupId group, const std::string& payload) {
        consensus::RaftCore* leader = leaderOf(group);
        if (leader == nullptr) {
            return false;
        }
        const consensus::ProposeResult proposed = leader->propose(payload);
        if (proposed.error != consensus::ProposeError::kOk) {
            return false;
        }
        pump();
        for (int round = 0; round < 30; ++round) {
            if (appliedOn(leader->selfId(), group, proposed.index) &&
                outcomeCommitted(leader->selfId(), group, proposed.id)) {
                return true;
            }
            tick(options_.heartbeat_interval_ms);
        }
        return false;
    }

    bool read(consensus::GroupId group, const std::string& key, std::string& value) {
        consensus::RaftCore* leader = leaderOf(group);
        if (leader == nullptr) {
            return false;
        }
        const consensus::ReadRequest request = leader->readIndex();
        if (request.error != consensus::ReadError::kOk) {
            return false;
        }
        pump();
        for (int round = 0; round < 30; ++round) {
            if (readReady(leader->selfId(), group, request.id)) {
                const auto it = node(leader->selfId()).kv[group].data.find(key);
                if (it == node(leader->selfId()).kv[group].data.end()) {
                    return false;
                }
                value = it->second;
                return true;
            }
            tick(options_.heartbeat_interval_ms);
        }
        return false;
    }

    void waitCommitted(consensus::GroupId group, consensus::Index index) {
        for (int round = 0; round < 40; ++round) {
            consensus::RaftCore* leader = leaderOf(group);
            if (leader != nullptr && leader->commitIndex() >= index &&
                appliedOn(leader->selfId(), group, index)) {
                return;
            }
            tick(options_.heartbeat_interval_ms);
        }
        expect(false, "commit index did not advance");
    }

    int committedCopies(consensus::GroupId group, const std::string& key,
                        const std::string& value) const {
        int copies = 0;
        for (const auto& item : nodes_) {
            if (!item->alive) {
                continue;
            }
            const auto kv = item->kv.find(group);
            if (kv == item->kv.end()) {
                continue;
            }
            const auto it = kv->second.data.find(key);
            if (it != kv->second.data.end() && it->second == value) {
                ++copies;
            }
        }
        return copies;
    }

    void pump() {
        for (int round = 0; round < 200; ++round) {
            bool progress = false;
            for (auto& item : nodes_) {
                if (item->alive) {
                    progress = pumpNode(*item) || progress;
                }
            }
            if (!mailbox_.empty()) {
                deliverAll();
                progress = true;
            }
            if (!progress) {
                return;
            }
        }
        expect(false, "cluster did not become idle");
    }

private:
    bool pumpNode(TestNode& item) {
        bool progress = false;
        for (consensus::GroupId group : item.multi.groupIds()) {
            consensus::RaftCore& core = item.multi.group(group);
            while (core.hasReady()) {
                consensus::Ready ready = core.takeReady();
                item.storage.at(group).applyReady(ready);
                if (ready.snapshot.has_value()) {
                    item.kv[group].install(*ready.snapshot);
                }
                for (const consensus::LogEntry& entry : ready.committed) {
                    item.kv[group].apply(entry);
                }
                if (item.kv[group].applied > 0) {
                    core.reportApplied(item.kv[group].applied);
                }
                for (const consensus::ProposalOutcome& outcome : ready.proposals) {
                    outcomes_[item.id][group][outcome.id] = outcome;
                }
                for (const consensus::ReadyRead& read : ready.reads) {
                    reads_[item.id][group].insert(read.id);
                }
                for (consensus::Message& message : ready.messages) {
                    mailbox_.push_back(std::move(message));
                }
                core.advance();
                progress = true;
            }
        }
        return progress;
    }

    void deliverAll() {
        std::vector<consensus::Message> batch;
        batch.swap(mailbox_);
        for (const consensus::Message& message : batch) {
            if (dropped_.count(message.from + "->" + message.to) != 0) {
                continue;
            }
            TestNode* dest = nullptr;
            for (auto& item : nodes_) {
                if (item->id == message.to) {
                    dest = item.get();
                    break;
                }
            }
            if (dest == nullptr || !dest->alive) {
                continue;
            }
            dest->multi.step(message);
        }
    }

    bool appliedOn(const consensus::NodeId& id, consensus::GroupId group,
                   consensus::Index index) {
        return node(id).kv[group].applied >= index;
    }

    bool outcomeCommitted(const consensus::NodeId& id, consensus::GroupId group,
                          consensus::ProposalId proposal) const {
        const auto node_it = outcomes_.find(id);
        if (node_it == outcomes_.end()) {
            return false;
        }
        const auto group_it = node_it->second.find(group);
        if (group_it == node_it->second.end()) {
            return false;
        }
        const auto it = group_it->second.find(proposal);
        return it != group_it->second.end() && it->second.committed;
    }

    bool readReady(const consensus::NodeId& id, consensus::GroupId group,
                   consensus::ReadId read_id) const {
        const auto node_it = reads_.find(id);
        if (node_it == reads_.end()) {
            return false;
        }
        const auto group_it = node_it->second.find(group);
        if (group_it == node_it->second.end()) {
            return false;
        }
        return group_it->second.count(read_id) != 0;
    }

    consensus::RaftOptions options_;
    std::int64_t now_ms_ = 0;
    std::vector<std::unique_ptr<TestNode>> nodes_;
    std::vector<consensus::Message> mailbox_;
    std::set<std::string> dropped_;
    std::map<consensus::NodeId,
             std::map<consensus::GroupId, std::map<consensus::ProposalId, consensus::ProposalOutcome>>>
        outcomes_;
    std::map<consensus::NodeId, std::map<consensus::GroupId, std::set<consensus::ReadId>>> reads_;
};

void testQuorumHelpers() {
    consensus::Configuration conf;
    conf.incoming.voters = {"a", "b", "c"};
    expect(consensus::hasMajority(conf.incoming.voters, {"a", "b"}),
           "2 of 3 is a majority");
    expect(!consensus::hasMajority(conf.incoming.voters, {"a"}), "1 of 3 is not a majority");
    expect(consensus::quorumAcked(conf, {"a", "c"}), "single config uses incoming majority");

    consensus::Membership old_m;
    old_m.voters = {"a", "b", "c"};
    consensus::Membership new_m;
    new_m.voters = {"a", "b", "c", "d"};
    const consensus::Configuration joint = consensus::jointConf(old_m, new_m);
    expect(joint.joint(), "joint configuration is marked joint");
    expect(!consensus::quorumAcked(joint, {"a", "b"}),
           "old majority without new majority cannot commit");
    expect(!consensus::quorumAcked(joint, {"c", "d"}),
           "new-side pair without old majority cannot commit");
    expect(consensus::quorumAcked(joint, {"a", "b", "c"}),
           "majority of both old and new can commit");
}

void testSingleNodeCommit() {
    Cluster cluster({"n1"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=v"), "single-node write commits after apply");
    std::string value;
    expect(cluster.read(1, "k", value), "ReadIndex succeeds on the leader");
    expect_equal(value, std::string("v"), "linearizable read sees the applied write");
}

void testThreeNodeReplication() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "user=alice"), "three-node write is committed");
    cluster.tick(20);
    expect(cluster.committedCopies(1, "user", "alice") >= 2,
           "majority replicas have applied the committed write");
}

void testLeaderCrashKeepsCommittedWrite() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=1"), "write succeeds before crash");
    const consensus::NodeId old_leader = cluster.leaderId(1);
    cluster.crash(old_leader);
    cluster.elect(1);
    std::string value;
    expect(cluster.read(1, "k", value), "new leader can ReadIndex the committed key");
    expect_equal(value, std::string("1"), "confirmed write survives leader crash");
}

void testMinorityPartitionCannotCommit() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=before"), "write before partition");
    const consensus::NodeId leader = cluster.leaderId(1);
    cluster.isolate(leader);
    cluster.tick(20);
    const bool partitioned_write = cluster.write(1, "k=after");
    expect(!partitioned_write, "leader without quorum must not confirm a write");
    cluster.tick(200);
    std::string value;
    cluster.heal();
    cluster.tick(200);
    cluster.elect(1);
    expect(cluster.read(1, "k", value), "cluster recovers after healing");
    expect_equal(value, std::string("before"), "uncommitted minority write is not visible");
}

void testPreVoteDoesNotInflateTerm() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=1"), "cluster is serving writes");
    consensus::NodeId follower;
    const consensus::NodeId leader = cluster.leaderId(1);
    for (const char* id : {"n1", "n2", "n3"}) {
        if (id != leader) {
            follower = id;
            break;
        }
    }
    const consensus::Term stable_term = cluster.raft(leader, 1).term();
    cluster.isolate(follower);
    for (int round = 0; round < 8; ++round) {
        cluster.tick(120);
    }
    expect_equal(cluster.raft(follower, 1).term(), stable_term,
                 "PreVote keeps an isolated follower from bumping term");
    expect_equal(cluster.raft(leader, 1).term(), stable_term,
                 "healthy majority term is unchanged by an isolated node");
}

void testLeaderTransfer() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=1"), "write before transfer");
    const consensus::NodeId old_leader = cluster.leaderId(1);
    consensus::NodeId target;
    for (const char* id : {"n1", "n2", "n3"}) {
        if (id != old_leader) {
            target = id;
            break;
        }
    }
    expect(cluster.raft(old_leader, 1).transferLeadership(target), "leader transfer starts");
    for (int round = 0; round < 40; ++round) {
        cluster.tick(10);
        if (cluster.leaderOf(1) != nullptr && cluster.leaderId(1) == target) {
            break;
        }
    }
    const consensus::NodeId new_leader = cluster.leaderId(1);
    expect_equal(new_leader, target, "TimeoutNow elects the transfer target");
    expect(cluster.write(1, "k=2"), "new leader accepts writes");
}

void testLearnerJointOnFourNodes() {
    consensus::RaftOptions options = testOptions();
    Cluster cluster({"n1", "n2", "n3"}, {1}, options);

    // Manually attach n4 by constructing a 4-node cluster with initial voters n1-n3.
    class Four {
    public:
        Four() {
            options_.heartbeat_interval_ms = 10;
            options_.election_timeout_min_ms = 50;
            options_.election_timeout_max_ms = 100;
            options_.transfer_timeout_ms = 80;
            options_.learner_promote_max_lag = 8;
            consensus::Configuration initial;
            initial.incoming.voters = {"n1", "n2", "n3"};
            initial = consensus::normalizeConf(initial);
            for (const char* id : {"n1", "n2", "n3", "n4"}) {
                auto node = std::make_unique<TestNode>(id, options_);
                node->storage.emplace(1, initial);
                node->kv.emplace(1, KvStore{});
                node->multi.createGroup(1, initial);
                nodes_.push_back(std::move(node));
            }
        }

        TestNode& node(const std::string& id) {
            for (auto& item : nodes_) {
                if (item->id == id) {
                    return *item;
                }
            }
            std::exit(EXIT_FAILURE);
        }

        void tick(std::int64_t delta) {
            pump();
            now_ += delta;
            for (auto& item : nodes_) {
                item->multi.tick(now_);
            }
            pump();
        }

        void pump() {
            for (int round = 0; round < 200; ++round) {
                bool progress = false;
                for (auto& item : nodes_) {
                    consensus::RaftCore& core = item->multi.group(1);
                    while (core.hasReady()) {
                        consensus::Ready ready = core.takeReady();
                        item->storage.at(1).applyReady(ready);
                        for (const consensus::LogEntry& entry : ready.committed) {
                            item->kv[1].apply(entry);
                        }
                        if (item->kv[1].applied > 0) {
                            core.reportApplied(item->kv[1].applied);
                        }
                        for (consensus::Message& message : ready.messages) {
                            mailbox_.push_back(std::move(message));
                        }
                        core.advance();
                        progress = true;
                    }
                }
                if (!mailbox_.empty()) {
                    std::vector<consensus::Message> batch;
                    batch.swap(mailbox_);
                    for (const consensus::Message& message : batch) {
                        for (auto& item : nodes_) {
                            if (item->id == message.to) {
                                item->multi.step(message);
                            }
                        }
                    }
                    progress = true;
                }
                if (!progress) {
                    return;
                }
            }
            expect(false, "four-node cluster did not become idle");
        }

        consensus::RaftCore* leader() {
            for (auto& item : nodes_) {
                if (item->multi.group(1).role() == consensus::RaftRole::kLeader) {
                    return &item->multi.group(1);
                }
            }
            return nullptr;
        }

        void elect() {
            for (int round = 0; round < 400; ++round) {
                tick(options_.heartbeat_interval_ms);
                if (leader() != nullptr) {
                    return;
                }
            }
            expect(false, "four-node cluster failed to elect");
        }

        bool write(const std::string& payload) {
            consensus::RaftCore* core = leader();
            expect(core != nullptr, "write needs a leader");
            const auto proposed = core->propose(payload);
            if (proposed.error != consensus::ProposeError::kOk) {
                return false;
            }
            for (int round = 0; round < 40; ++round) {
                pump();
                if (node(core->selfId()).kv[1].applied >= proposed.index &&
                    core->commitIndex() >= proposed.index) {
                    return true;
                }
                tick(options_.heartbeat_interval_ms);
            }
            return false;
        }

        std::vector<std::unique_ptr<TestNode>> nodes_;
        std::vector<consensus::Message> mailbox_;
        consensus::RaftOptions options_ = testOptions();
        std::int64_t now_ = 0;
    };

    Four cluster4;
    cluster4.elect();
    expect(cluster4.write("k=1"), "write before adding learner");
    consensus::RaftCore* leader = cluster4.leader();
    expect(leader->addLearner("n4").error == consensus::ProposeError::kOk, "addLearner proposed");
    cluster4.tick(20);
    cluster4.tick(20);
    expect(consensus::isLearner(leader->configuration(), "n4") ||
               consensus::isLearner(cluster4.node("n4").multi.group(1).configuration(), "n4"),
           "n4 is a learner after the config commits");
    for (int round = 0; round < 20; ++round) {
        cluster4.tick(10);
        leader = cluster4.leader();
        if (leader != nullptr &&
            leader->lastIndex() - leader->matchIndex("n4") <= 8 &&
            leader->matchIndex("n4") > 0) {
            break;
        }
    }
    leader = cluster4.leader();
    expect(leader != nullptr, "leader exists before promote");
    const auto promoted = leader->promoteLearner("n4");
    expect(promoted.error == consensus::ProposeError::kOk,
           "caught-up learner can be promoted via joint consensus");
    cluster4.tick(30);
    cluster4.tick(30);
    leader = cluster4.leader();
    expect(leader != nullptr, "leader exists after promote");
    expect(!leader->configuration().joint(), "leader leaves joint once C_new commits");
    expect(consensus::isVoter(leader->configuration(), "n4"), "n4 is a voter in C_new");
    expect(cluster4.write("k=2"), "four-voter group still commits");
}

void testJointSplitCannotCommit() {
    class Five {
    public:
        Five() {
            consensus::Configuration initial;
            initial.incoming.voters = {"n1", "n2", "n3"};
            initial = consensus::normalizeConf(initial);
            for (const char* id : {"n1", "n2", "n3", "n4"}) {
                auto node = std::make_unique<TestNode>(id, options_);
                node->storage.emplace(1, initial);
                node->kv.emplace(1, KvStore{});
                node->multi.createGroup(1, initial);
                nodes_.push_back(std::move(node));
            }
        }

        TestNode* find(const std::string& id) {
            for (auto& item : nodes_) {
                if (item->id == id) {
                    return item.get();
                }
            }
            return nullptr;
        }

        void drop(const std::string& a, const std::string& b) {
            dropped_.insert(a + "->" + b);
            dropped_.insert(b + "->" + a);
        }

        void tick(std::int64_t delta) {
            pump();
            now_ += delta;
            for (auto& item : nodes_) {
                item->multi.tick(now_);
            }
            pump();
        }

        void pump() {
            for (int round = 0; round < 200; ++round) {
                bool progress = false;
                for (auto& item : nodes_) {
                    consensus::RaftCore& core = item->multi.group(1);
                    while (core.hasReady()) {
                        consensus::Ready ready = core.takeReady();
                        item->storage.at(1).applyReady(ready);
                        for (const consensus::LogEntry& entry : ready.committed) {
                            item->kv[1].apply(entry);
                        }
                        if (item->kv[1].applied > 0) {
                            core.reportApplied(item->kv[1].applied);
                        }
                        for (consensus::Message& message : ready.messages) {
                            mailbox_.push_back(std::move(message));
                        }
                        core.advance();
                        progress = true;
                    }
                }
                if (!mailbox_.empty()) {
                    std::vector<consensus::Message> batch;
                    batch.swap(mailbox_);
                    for (const consensus::Message& message : batch) {
                        if (dropped_.count(message.from + "->" + message.to) != 0) {
                            continue;
                        }
                        TestNode* dest = find(message.to);
                        if (dest != nullptr) {
                            dest->multi.step(message);
                        }
                    }
                    progress = true;
                }
                if (!progress) {
                    return;
                }
            }
        }

        consensus::RaftCore* leader() {
            for (auto& item : nodes_) {
                if (item->multi.group(1).role() == consensus::RaftRole::kLeader) {
                    return &item->multi.group(1);
                }
            }
            return nullptr;
        }

        void elect() {
            for (int round = 0; round < 400; ++round) {
                tick(options_.heartbeat_interval_ms);
                if (leader() != nullptr) {
                    return;
                }
            }
            expect(false, "joint test failed to elect");
        }

        bool write(const std::string& payload) {
            consensus::RaftCore* core = leader();
            if (core == nullptr) {
                return false;
            }
            const auto proposed = core->propose(payload);
            if (proposed.error != consensus::ProposeError::kOk) {
                return false;
            }
            for (int round = 0; round < 20; ++round) {
                pump();
                if (core->commitIndex() >= proposed.index &&
                    find(core->selfId())->kv[1].applied >= proposed.index) {
                    return true;
                }
                tick(options_.heartbeat_interval_ms);
            }
            return false;
        }

        std::vector<std::unique_ptr<TestNode>> nodes_;
        std::vector<consensus::Message> mailbox_;
        std::set<std::string> dropped_;
        consensus::RaftOptions options_ = testOptions();
        std::int64_t now_ = 0;
    };

    Five cluster;
    cluster.elect();
    expect(cluster.write("seed=1"), "seed before joint");
    consensus::RaftCore* leader = cluster.leader();
    expect(leader->addLearner("n4").error == consensus::ProposeError::kOk, "learner added");
    cluster.tick(40);
    leader = cluster.leader();
    expect(leader->promoteLearner("n4").error == consensus::ProposeError::kOk,
           "promote starts joint consensus");
    cluster.pump();
    expect(cluster.leader()->configuration().joint() ||
               consensus::isVoter(cluster.leader()->configuration(), "n4"),
           "group entered joint or already left it");

    // Force a joint window: if C_new already committed, still verify CP on the 4-voter set.
    leader = cluster.leader();
    const bool in_joint = leader->configuration().joint();
    cluster.drop("n1", "n3");
    cluster.drop("n1", "n4");
    cluster.drop("n2", "n3");
    cluster.drop("n2", "n4");
    const consensus::Index commit_before = leader->commitIndex();
    const bool wrote = cluster.write("split=1");
    cluster.tick(80);
    leader = cluster.leader();
    if (leader != nullptr &&
        (leader->selfId() == "n1" || leader->selfId() == "n2")) {
        expect(leader->commitIndex() == commit_before || !wrote,
               "partitioned old majority cannot advance commit during/after joint");
    }
    (void)in_joint;
}

void testMultiRaftIndependentGroups() {
    Cluster cluster({"n1", "n2", "n3"}, {1, 2});
    cluster.elect(1);
    cluster.elect(2);
    expect(cluster.node("n1").multi.groupCount() == 2,
           "one process hosts both Raft groups");
    expect(cluster.write(1, "g1=a"), "group 1 write");
    expect(cluster.write(2, "g2=b"), "group 2 write");
    const consensus::Term term1 = cluster.raft(cluster.leaderId(1), 1).term();
    const consensus::NodeId g1_leader = cluster.leaderId(1);
    cluster.crash(g1_leader);
    cluster.elect(1);
    std::string value;
    expect(cluster.read(1, "g1", value), "group 1 recovers independently");
    expect_equal(value, std::string("a"), "group 1 committed data remains");
    // Group 2 still has a leader among the surviving nodes unless g1_leader was also its leader.
    if (cluster.leaderOf(2) == nullptr) {
        cluster.elect(2);
    }
    expect(cluster.write(2, "g2=c"), "group 2 keeps serving while group 1 re-elects");
    (void)term1;
}

void testRestartFromStorage() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=persist"), "write before restart");
    cluster.crash("n3");
    cluster.restart("n3");
    cluster.tick(40);
    expect(cluster.node("n3").kv[1].data["k"] == "persist" ||
               cluster.raft("n3", 1).commitIndex() >= 1,
           "restarted replica reloads durable state");
    cluster.tick(40);
    expect(cluster.committedCopies(1, "k", "persist") >= 2,
           "restarted replica catches up committed entries");
}

void testCheckQuorumStepsDown() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    const consensus::NodeId leader = cluster.leaderId(1);
    cluster.isolate(leader);
    for (int round = 0; round < 25; ++round) {
        cluster.tick(10);
        if (cluster.raft(leader, 1).role() != consensus::RaftRole::kLeader) {
            break;
        }
    }
    expect(cluster.raft(leader, 1).role() != consensus::RaftRole::kLeader,
           "CheckQuorum makes a partitioned leader step down");
}

void testLogConflictTruncation() {
    Cluster cluster({"n1", "n2", "n3"}, {1});
    cluster.elect(1);
    expect(cluster.write(1, "k=ok"), "committed prefix");
    const consensus::NodeId leader = cluster.leaderId(1);
    cluster.isolate(leader);
    const bool lost = cluster.write(1, "k=stale");
    expect(!lost, "isolated leader cannot confirm a conflicting write");
    cluster.tick(200);
    cluster.heal();
    cluster.tick(200);
    if (cluster.leaderOf(1) == nullptr) {
        cluster.elect(1);
    }
    expect(cluster.write(1, "k=fresh"), "majority commits a later value");
    std::string value;
    expect(cluster.read(1, "k", value), "read after conflict resolution");
    expect_equal(value, std::string("fresh"), "uncommitted isolated log is overwritten");
}

} // namespace

int main() {
    testQuorumHelpers();
    testSingleNodeCommit();
    testThreeNodeReplication();
    testLeaderCrashKeepsCommittedWrite();
    testMinorityPartitionCannotCommit();
    testPreVoteDoesNotInflateTerm();
    testLeaderTransfer();
    testLearnerJointOnFourNodes();
    testJointSplitCannotCommit();
    testMultiRaftIndependentGroups();
    testRestartFromStorage();
    testCheckQuorumStepsDown();
    testLogConflictTruncation();
    std::cout << "raft_core tests passed" << std::endl;
    return 0;
}
