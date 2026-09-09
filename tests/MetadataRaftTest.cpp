#include "mini_redis/cluster/MetadataController.hpp"
#include "mini_redis/cluster/MetadataWatcher.hpp"
#include "mini_redis/consensus/MemoryStorage.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr consensus::GroupId kDataGroup = 1;

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

consensus::RaftOptions options() {
    consensus::RaftOptions result;
    result.heartbeat_interval_ms = 10;
    result.election_timeout_min_ms = 50;
    result.election_timeout_max_ms = 100;
    result.check_quorum = true;
    result.pre_vote = true;
    return result;
}

consensus::Configuration configuration() {
    consensus::Configuration result;
    result.incoming.voters = {"n1", "n2", "n3"};
    return consensus::normalizeConf(std::move(result));
}

cluster::MetadataNodeRecord metadataNode(const std::string& id,
                                         std::uint16_t port) {
    cluster::MetadataNodeRecord node;
    node.id = id;
    node.client = {"127.0.0.1", port};
    node.internal = {"127.0.0.1", static_cast<std::uint16_t>(port + 1000)};
    node.admin = {"127.0.0.1", static_cast<std::uint16_t>(port + 2000)};
    node.failure_domain = id;
    return node;
}

struct Node {
    consensus::NodeId id;
    consensus::MultiRaft multi;
    std::map<consensus::GroupId, consensus::MemoryStorage> storage;
    std::map<consensus::GroupId, consensus::Index> applied;
    std::vector<std::string> data_commands;
    cluster::ClusterRouter router;
    cluster::MetadataWatcher watcher;
    cluster::MetadataStateMachine metadata;
    cluster::MetadataController controller;

    Node(consensus::NodeId node_id, const consensus::Configuration& conf,
         const consensus::RaftOptions& raft_options)
        : id(std::move(node_id)),
          multi(id, raft_options),
          router(id, {}),
          watcher(router),
          controller(multi, metadata, &watcher) {
        for (consensus::GroupId group :
             {consensus::kMetadataGroupId, kDataGroup}) {
            storage.emplace(group, conf);
            applied[group] = 0;
            multi.createGroup(group, conf);
        }
    }
};

class Cluster {
public:
    Cluster() {
        const consensus::Configuration conf = configuration();
        const consensus::RaftOptions raft_options = options();
        for (const char* id : {"n1", "n2", "n3"}) {
            nodes_.push_back(
                std::make_unique<Node>(id, conf, raft_options));
        }
    }

    Node& node(const consensus::NodeId& id) {
        for (auto& node : nodes_) {
            if (node->id == id) return *node;
        }
        std::cerr << "unknown node " << id << std::endl;
        std::exit(EXIT_FAILURE);
    }

    consensus::RaftCore* leader(consensus::GroupId group) {
        for (auto& node : nodes_) {
            consensus::RaftCore& core = node->multi.group(group);
            if (core.role() == consensus::RaftRole::kLeader) return &core;
        }
        return nullptr;
    }

    void elect(consensus::GroupId group) {
        for (int round = 0; round < 100 && leader(group) == nullptr; ++round) {
            tick(10);
        }
        expect(leader(group) != nullptr, "group elects a leader");
    }

    void tick(std::int64_t delta_ms) {
        pump();
        now_ms_ += delta_ms;
        for (auto& node : nodes_) node->multi.tick(now_ms_);
        pump();
    }

    void pump() {
        for (int round = 0; round < 300; ++round) {
            bool progress = false;
            for (auto& node : nodes_) {
                for (consensus::GroupId group : node->multi.groupIds()) {
                    consensus::RaftCore& core = node->multi.group(group);
                    while (core.hasReady()) {
                        consensus::Ready ready = core.takeReady();
                        node->storage.at(group).applyReady(ready);
                        if (ready.snapshot.has_value() &&
                            group == consensus::kMetadataGroupId) {
                            std::string error;
                            expect(node->controller.installSnapshot(
                                       *ready.snapshot, error),
                                   "metadata snapshot installs");
                        }
                        for (const consensus::LogEntry& entry : ready.committed) {
                            if (entry.type == consensus::EntryType::kCommand) {
                                if (group == consensus::kMetadataGroupId) {
                                    node->controller.applyCommitted(entry);
                                } else {
                                    node->data_commands.push_back(entry.payload);
                                }
                            }
                            node->applied[group] = entry.index;
                        }
                        if (node->applied[group] > 0) {
                            core.reportApplied(node->applied[group]);
                        }
                        for (consensus::Message& message : ready.messages) {
                            mailbox_.push_back(std::move(message));
                        }
                        core.advance();
                        progress = true;
                    }
                }
            }
            if (!mailbox_.empty()) {
                std::vector<consensus::Message> batch;
                batch.swap(mailbox_);
                for (const consensus::Message& message : batch) {
                    if (blocked_groups_.count(message.group) == 0) {
                        node(message.to).multi.step(message);
                    }
                }
                progress = true;
            }
            if (!progress) return;
        }
        expect(false, "cluster pump becomes idle");
    }

    bool submit(cluster::MetadataCommand command) {
        consensus::RaftCore* metadata_leader =
            leader(consensus::kMetadataGroupId);
        if (metadata_leader == nullptr) return false;
        Node& leader_node = node(metadata_leader->selfId());
        const std::uint64_t target_revision =
            leader_node.metadata.metadata().revision + 1;
        const consensus::ProposeResult proposed =
            leader_node.controller.propose(command);
        if (proposed.error != consensus::ProposeError::kOk) return false;
        for (int round = 0; round < 50; ++round) {
            pump();
            if (leader_node.metadata.metadata().revision >= target_revision) {
                return true;
            }
            tick(10);
        }
        return false;
    }

    bool writeData(const std::string& payload) {
        consensus::RaftCore* data_leader = leader(kDataGroup);
        if (data_leader == nullptr) return false;
        const consensus::ProposeResult proposed = data_leader->propose(payload);
        if (proposed.error != consensus::ProposeError::kOk) return false;
        for (int round = 0; round < 50; ++round) {
            pump();
            if (node(data_leader->selfId()).applied[kDataGroup] >=
                proposed.index) {
                return true;
            }
            tick(10);
        }
        return false;
    }

    void block(consensus::GroupId group) { blocked_groups_.insert(group); }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<consensus::Message> mailbox_;
    std::set<consensus::GroupId> blocked_groups_;
    std::int64_t now_ms_ = 0;
};

cluster::MetadataCommand makeCommand(cluster::MetadataOperation operation,
                                     std::string request_id,
                                     std::uint64_t revision) {
    cluster::MetadataCommand result;
    result.operation = operation;
    result.request_id = std::move(request_id);
    result.expected_revision = revision;
    return result;
}

} // namespace

int main() {
    Cluster cluster;
    cluster.elect(consensus::kMetadataGroupId);
    cluster.elect(kDataGroup);

    std::uint64_t revision = 0;
    cluster::MetadataCommand initialize =
        makeCommand(cluster::MetadataOperation::kInitializeCluster, "init", revision++);
    initialize.cluster_id = "raft-cluster";
    expect(cluster.submit(initialize), "cluster id commits through metadata Raft");

    for (int index = 1; index <= 3; ++index) {
        cluster::MetadataCommand add =
            makeCommand(cluster::MetadataOperation::kUpsertNode,
                        "node-" + std::to_string(index), revision++);
        add.node = metadataNode("n" + std::to_string(index),
                                static_cast<std::uint16_t>(7000 + index));
        expect(cluster.submit(add), "node record commits through metadata Raft");
    }

    cluster::MetadataCommand add_group =
        makeCommand(cluster::MetadataOperation::kUpsertGroup, "data-group", revision++);
    add_group.group.id = 1;
    add_group.group.voters = {"n1", "n2", "n3"};
    expect(cluster.submit(add_group), "data group record commits");

    cluster::MetadataCommand assign =
        makeCommand(cluster::MetadataOperation::kAssignSlots, "all-slots", revision++);
    assign.slot_start = 0;
    assign.slot_end = cluster::kSlotCount - 1;
    assign.owner_group = 1;
    assign.expected_ownership_epoch = 0;
    assign.new_ownership_epoch = 1;
    expect(cluster.submit(assign), "slot ownership commits");
    for (int round = 0; round < 5; ++round) cluster.tick(10);
    for (const char* id : {"n1", "n2", "n3"}) {
        expect(cluster.node(id).metadata.metadata().revision == revision,
               "committed metadata reaches every watcher");
    }

    const consensus::NodeId data_leader = cluster.leader(kDataGroup)->selfId();
    cluster::TopologyBuilder routed_builder;
    routed_builder.setTopologyRevision(revision)
        .addNode("n1", "127.0.0.1", 7001)
        .addNode("n2", "127.0.0.1", 7002)
        .addNode("n3", "127.0.0.1", 7003)
        .addShard(1, {"n1", "n2", "n3"})
        .assignSlots(1, {0, cluster::kSlotCount - 1})
        .setSlotEpoch(0, 1)
        .setLeaderHint(1, data_leader, 1,
                       std::numeric_limits<std::int64_t>::max());
    // Explicit per-slot epochs are already 1 by default at this revision.
    const cluster::TopologyPtr last_committed = routed_builder.build();
    for (const char* id : {"n1", "n2", "n3"}) {
        expect(cluster.node(id).router.publishTopology(last_committed),
               "last committed topology publishes to every data node");
    }

    const std::uint64_t committed_revision = revision;
    cluster.block(consensus::kMetadataGroupId);

    consensus::RaftCore* old_metadata_leader =
        cluster.leader(consensus::kMetadataGroupId);
    expect(old_metadata_leader != nullptr, "metadata leader exists before partition");
    cluster::MetadataCommand unavailable_change =
        makeCommand(cluster::MetadataOperation::kUpsertNode, "partitioned-change",
                    committed_revision);
    unavailable_change.node = metadataNode("n4", 7004);
    const consensus::ProposeResult pending =
        cluster.node(old_metadata_leader->selfId())
            .controller.propose(unavailable_change);
    expect(pending.error == consensus::ProposeError::kOk,
           "old leader may accept but cannot commit a management proposal");
    for (int round = 0; round < 20; ++round) cluster.tick(10);

    expect(cluster.leader(consensus::kMetadataGroupId) == nullptr,
           "metadata group loses leadership without a majority");
    for (const char* id : {"n1", "n2", "n3"}) {
        expect(cluster.node(id).metadata.metadata().revision == committed_revision,
               "uncommitted management change is invisible");
        expect(cluster.node(id).router.topology()->topologyRevision() ==
                   committed_revision,
               "data nodes retain the last committed topology");
    }

    expect(cluster.writeData("stable-shard-write"),
           "data Raft group still commits while metadata quorum is unavailable");
    cluster::ClientSession session;
    const cluster::RouteDecision route =
        cluster.node(data_leader).router.routeKeys(
            {"stable-key"}, session, 1, 1000);
    expect(route.action == cluster::RouteAction::kLocal &&
               route.shard == 1 && route.slot_epoch == 1,
           "stable shard routing does not consult the unavailable control plane");

    std::cout << "Metadata Raft tests passed" << std::endl;
    return EXIT_SUCCESS;
}
