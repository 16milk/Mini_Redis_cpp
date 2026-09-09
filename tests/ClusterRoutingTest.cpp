#include "mini_redis/cluster/ClusterConfig.hpp"
#include "mini_redis/cluster/Router.hpp"
#include "mini_redis/cluster/Slot.hpp"
#include "mini_redis/cluster/Topology.hpp"
#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"

#include <cstdlib>
#include <atomic>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expect_equal(const std::string& actual, const std::string& expected,
                  const std::string& description) {
    if (actual != expected) {
        std::cerr << "FAILED: " << description << "\nExpected: " << expected
                  << "\nActual:   " << actual << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expect_contains(const std::string& haystack, const std::string& needle,
                     const std::string& description) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "FAILED: " << description << "\nExpected to contain: " << needle
                  << "\nActual:              " << haystack << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::string> toArguments(std::initializer_list<const char*> values) {
    std::vector<std::string> arguments;
    arguments.reserve(values.size());
    for (const char* value : values) {
        arguments.emplace_back(value);
    }
    return arguments;
}

// One simulated data node: its own keyspace, its own view of the topology and a
// client session that carries the one-shot ASKING flag.
class Node {
public:
    Node(cluster::NodeId node_id, cluster::TopologyPtr topology, const UnixMillis* clock)
        : database_(true, [clock] { return *clock; }),
          router_(std::move(node_id), std::move(topology)),
          commands_(database_, &router_) {}

    std::string run(std::initializer_list<const char*> values) {
        return commands_.execute(toArguments(values), session_);
    }

    Database& database() { return database_; }
    cluster::ClusterRouter& router() { return router_; }

private:
    Database database_;
    cluster::ClusterRouter router_;
    CommandHandler commands_;
    cluster::ClientSession session_;
};

// Three nodes, two logical shards. Each shard is one Raft group replicated on
// all three nodes, so the cluster runs 2 Raft groups for 16384 slots.
cluster::TopologyBuilder baseCluster() {
    cluster::TopologyBuilder builder;
    builder.setConfigEpoch(7)
        .addNode("n1", "127.0.0.1", 7001)
        .addNode("n2", "127.0.0.1", 7002)
        .addNode("n3", "127.0.0.1", 7003)
        .addShard(1, {"n1", "n2", "n3"})
        .addShard(2, {"n2", "n3", "n1"});

    const std::vector<cluster::SlotRange> ranges = cluster::splitSlotsEvenly(2);
    builder.assignSlots(1, ranges[0]).assignSlots(2, ranges[1]);
    return builder;
}

// A static test topology stands in for heartbeat-refreshed leader hints.
constexpr std::int64_t kNeverExpires = std::numeric_limits<std::int64_t>::max();

cluster::TopologyBuilder stableCluster() {
    cluster::TopologyBuilder builder = baseCluster();
    builder.setLeaderHint(1, "n1", 4, kNeverExpires)
        .setLeaderHint(2, "n2", 9, kNeverExpires);
    return builder;
}

std::string nodeTuple(const std::string& host, int port, const std::string& node_id) {
    return "*3\r\n$" + std::to_string(host.size()) + "\r\n" + host + "\r\n:" +
           std::to_string(port) + "\r\n$" + std::to_string(node_id.size()) + "\r\n" +
           node_id + "\r\n";
}

} // namespace

int main() {
    UnixMillis clock = 1'700'000'000'000;

    // Slot numbers the assertions below depend on.
    expect(cluster::keyToSlot("hello") == 866, "hello lives in slot 866 (shard 1)");
    expect(cluster::keyToSlot("foo") == 12182, "foo lives in slot 12182 (shard 2)");
    expect(cluster::keyToSlot("bar") == 5061, "bar lives in slot 5061 (shard 1)");
    expect(cluster::keyToSlot("{hello}:a") == 866, "the hash tag pins the slot");

    // ---------------------------------------------------------------------
    // Topology: 16384 slots, 2 Raft groups.
    // ---------------------------------------------------------------------
    const cluster::TopologyPtr stable = stableCluster().build();
    expect(stable->allSlotsAssigned(), "the two shards cover all 16384 slots");
    expect(stable->raftGroupCount() == 2,
           "16384 routing slots are served by only 2 Raft groups");
    expect(stable->slotOwner(866) == 1, "slot 866 belongs to shard 1");
    expect(stable->slotOwner(12182) == 2, "slot 12182 belongs to shard 2");
    expect(stable->ownedRanges(1).size() == 1, "shard 1 owns one contiguous range");
    expect(stable->trustedLeader(1, clock).value() == "n1", "shard 1 leader hint");

    // ---------------------------------------------------------------------
    // Owner-leader executes locally, everything else is redirected.
    // ---------------------------------------------------------------------
    Node n1("n1", stable, &clock);
    expect_equal(n1.run({"SET", "hello", "world"}), "+OK\r\n",
                 "the leader of the owning shard serves the write locally");
    expect_equal(n1.run({"GET", "hello"}), "$5\r\nworld\r\n",
                 "and the matching read as well");
    expect_equal(n1.run({"GET", "foo"}), "-MOVED 12182 127.0.0.1:7002\r\n",
                 "a key owned by the other shard returns MOVED to that shard's leader");
    expect_equal(n1.run({"SET", "foo", "x"}), "-MOVED 12182 127.0.0.1:7002\r\n",
                 "MOVED applies to writes too");

    // A node that replicates the shard but is not its Raft leader still
    // redirects: writes need the leader to propose, strong reads need ReadIndex.
    Node n3("n3", stable, &clock);
    expect(stable->nodeHostsShard("n3", 1), "n3 is a voter of shard 1");
    expect_equal(n3.run({"GET", "hello"}), "-MOVED 866 127.0.0.1:7001\r\n",
                 "a follower of the owning group redirects to the current leader");
    expect_equal(n3.run({"GET", "foo"}), "-MOVED 12182 127.0.0.1:7002\r\n",
                 "n3 redirects shard 2 keys to n2");

    // ---------------------------------------------------------------------
    // Commands without keys, and commands cluster mode refuses.
    // ---------------------------------------------------------------------
    expect_equal(n1.run({"PING"}), "+PONG\r\n",
                 "a keyless command runs locally on any healthy node");
    expect_equal(n3.run({"PING"}), "+PONG\r\n", "including on a pure follower");
    expect_equal(n1.run({"KEYS", "*"}),
                 "-ERR KEYS is not available in cluster mode\r\n",
                 "KEYS has no global instant in a cluster");
    expect_equal(n1.run({"SAVE"}), "-ERR SAVE is not available in cluster mode\r\n",
                 "SAVE is replaced by an authenticated admin snapshot API");
    expect_equal(n1.run({"NOSUCHCMD", "hello"}),
                 "-ERR unknown command `NOSUCHCMD`\r\n",
                 "unknown commands keep the single-node error message");

    // ---------------------------------------------------------------------
    // Multi-key commands must stay inside one slot.
    // ---------------------------------------------------------------------
    expect_equal(n1.run({"DEL", "foo", "bar"}),
                 "-CROSSSLOT Keys in request don't hash to the same slot\r\n",
                 "DEL across two slots is rejected before any execution");
    expect_equal(n1.run({"EXISTS", "hello", "foo"}),
                 "-CROSSSLOT Keys in request don't hash to the same slot\r\n",
                 "EXISTS is a read but still confined to one slot");
    expect_equal(n1.run({"SET", "{hello}:a", "1"}), "+OK\r\n",
                 "a tagged key lands in the same slot as its tag");
    expect_equal(n1.run({"EXISTS", "{hello}:a", "hello"}), ":2\r\n",
                 "same-slot multi-key commands execute as one operation");
    expect_equal(n1.run({"DEL", "{hello}:a", "{hello}:b"}), ":1\r\n",
                 "a same-slot DEL is applied as a single log record");
    // The CROSSSLOT check comes before ownership, so even a shard-2 pair fails here.
    expect_equal(n1.run({"DEL", "{user:42}:profile", "foo"}),
                 "-CROSSSLOT Keys in request don't hash to the same slot\r\n",
                 "CROSSSLOT is decided from the keys alone");

    // ---------------------------------------------------------------------
    // CLUSTER SLOTS: committed slot ranges plus the trusted leader.
    // ---------------------------------------------------------------------
    const std::string node1 = nodeTuple("127.0.0.1", 7001, "n1");
    const std::string node2 = nodeTuple("127.0.0.1", 7002, "n2");
    const std::string node3 = nodeTuple("127.0.0.1", 7003, "n3");
    const std::string expected_slots =
        "*2\r\n"
        "*5\r\n:0\r\n:8191\r\n" + node1 + node2 + node3 +
        "*5\r\n:8192\r\n:16383\r\n" + node2 + node3 + node1;
    expect_equal(n1.run({"CLUSTER", "SLOTS"}), expected_slots,
                 "CLUSTER SLOTS lists each range as [start, end, primary, replicas...]");
    expect_equal(n3.run({"CLUSTER", "SLOTS"}), expected_slots,
                 "every node reports the same committed topology");
    expect_equal(n1.run({"CLUSTER", "KEYSLOT", "{user:42}:orders"}), ":15880\r\n",
                 "CLUSTER KEYSLOT exposes the routing function");
    expect_equal(n1.run({"CLUSTER", "MYID"}), "$2\r\nn1\r\n", "CLUSTER MYID");

    const std::string info = n1.run({"CLUSTER", "INFO"});
    expect_contains(info, "cluster_state:ok", "all slots covered and every group led");
    expect_contains(info, "cluster_slots_assigned:16384", "full slot coverage");
    expect_contains(info, "cluster_slot_space:16384", "the routing space is fixed");
    expect_contains(info, "cluster_raft_groups:2", "but only two Raft groups exist");
    expect_contains(info, "cluster_my_leader_shards:1", "n1 leads exactly one shard");

    // ---------------------------------------------------------------------
    // Migration window: source answers ASK, target consumes one ASKING.
    // ---------------------------------------------------------------------
    // Slot 866 is moving from shard 1 (led by n1) to shard 2 (led by n2). The
    // committed owner is still shard 1 until the handover is committed.
    const cluster::TopologyPtr migrating =
        stableCluster().setMigration(866, 1, 2, true).build();

    Node source("n1", migrating, &clock);
    Node target("n2", migrating, &clock);

    // The ASK window opens only after the source is fenced, and a fenced source
    // has stopped applying writes for this slot. Whatever it still holds is a
    // frozen snapshot, while the target is already accepting writes through
    // ASKING. Answering from the local copy would hand back a value that has
    // since been overwritten, so the handover is all-or-nothing per slot rather
    // than per key.
    source.database().set("hello", "stale-after-fence");
    expect_equal(source.run({"GET", "hello"}), "-ASK 866 127.0.0.1:7002\r\n",
                 "a fenced source redirects even keys it still physically holds");
    expect_equal(source.run({"SET", "hello", "nope"}), "-ASK 866 127.0.0.1:7002\r\n",
                 "and refuses writes to them");
    expect_equal(source.run({"GET", "{hello}:moved"}), "-ASK 866 127.0.0.1:7002\r\n",
                 "a key the source no longer holds produces a one-shot ASK");
    expect_equal(source.run({"SET", "{hello}:fresh", "1"}), "-ASK 866 127.0.0.1:7002\r\n",
                 "creating a new key in a migrating slot also belongs to the target");
    expect_equal(source.run({"EXISTS", "hello", "{hello}:moved"}),
                 "-ASK 866 127.0.0.1:7002\r\n",
                 "multi-key commands move as a unit, because the slot does");

    expect_equal(target.run({"GET", "{hello}:moved"}), "-MOVED 866 127.0.0.1:7001\r\n",
                 "without ASKING the target still points back at the committed owner");
    expect_equal(target.run({"ASKING"}), "+OK\r\n", "ASKING is accepted");
    expect_equal(target.run({"SET", "{hello}:moved", "arrived"}), "+OK\r\n",
                 "the command right after ASKING is served by the importing target");
    expect_equal(target.run({"GET", "{hello}:moved"}), "-MOVED 866 127.0.0.1:7001\r\n",
                 "the flag is one-shot: the next command is redirected again");

    // ASKING alone grants nothing, and it does not leak to another slot.
    expect_equal(target.run({"ASKING"}), "+OK\r\n", "ASKING again");
    expect_equal(target.run({"PING"}), "+PONG\r\n",
                 "a keyless command also consumes the flag");
    expect_equal(target.run({"GET", "{hello}:moved"}), "-MOVED 866 127.0.0.1:7001\r\n",
                 "so the following command no longer has ASK permission");
    expect_equal(target.run({"ASKING"}), "+OK\r\n", "ASKING once more");
    expect_equal(target.run({"GET", "hello2"}),
                 "-MOVED " + std::to_string(cluster::keyToSlot("hello2")) +
                     " 127.0.0.1:7001\r\n",
                 "ASKING never applies to a slot that is not being imported");

    // The importing side holds the whole slot as of the fence, so a key that is
    // absent is genuinely absent rather than "not copied yet". Treating absence
    // as incompleteness would leave lookups of never-written keys retrying for
    // as long as the migration record exists.
    target.database().set("{hello}:here", "1");
    expect_equal(target.run({"ASKING"}), "+OK\r\n", "ASKING before a multi-key command");
    expect_equal(target.run({"EXISTS", "{hello}:here", "{hello}:notyet"}), ":1\r\n",
                 "the target answers for the whole slot, missing keys included");

    // A node that leads both the source and the target group must not ASK itself.
    const cluster::TopologyPtr self_migration = baseCluster()
                                                    .setLeaderHint(1, "n2", 4, kNeverExpires)
                                                    .setLeaderHint(2, "n2", 9, kNeverExpires)
                                                    .setMigration(866, 1, 2, true)
                                                    .build();
    Node both("n2", self_migration, &clock);
    expect_equal(both.run({"GET", "{hello}:moved"}), "$-1\r\n",
                 "leading both groups means executing locally, not redirecting to self");

    // Before the source commits TargetReady the ASK window is still closed.
    const cluster::TopologyPtr pending =
        stableCluster().setMigration(866, 1, 2, false).build();
    Node pending_source("n1", pending, &clock);
    Node pending_target("n2", pending, &clock);
    expect_equal(pending_source.run({"GET", "{hello}:moved"}), "$-1\r\n",
                 "the source answers locally until the target is proven ready");
    expect_equal(pending_target.run({"ASKING"}), "+OK\r\n", "ASKING is still accepted");
    expect_equal(pending_target.run({"GET", "{hello}:moved"}),
                 "-MOVED 866 127.0.0.1:7001\r\n",
                 "but the target refuses to consume it before its Raft state says so");

    // ---------------------------------------------------------------------
    // Unknown leader and uncovered slots are distinct, explicit answers.
    // ---------------------------------------------------------------------
    const cluster::TopologyPtr expired_lease = baseCluster()
                                                   .setLeaderHint(1, "n1", 4, 1'000)
                                                   .setLeaderHint(2, "n2", 9, 1'000)
                                                   .build();
    Node stale("n1", expired_lease, &clock);
    expect_equal(stale.run({"GET", "hello"}),
                 "-TRYAGAIN Slot leader unknown, retry after topology refresh\r\n",
                 "an expired leader hint must not be turned into a guessed address");
    expect_equal(stale.run({"CLUSTER", "SLOTS"}),
                 "-TRYAGAIN Leader unknown for shard 1\r\n",
                 "CLUSTER SLOTS returns TRYAGAIN rather than half a slot table");

    cluster::TopologyBuilder partial_builder;
    partial_builder.addNode("n1", "127.0.0.1", 7001)
        .addShard(1, {"n1"})
        .assignSlots(1, cluster::SlotRange{0, 1000})
        .setLeaderHint(1, "n1", 1, kNeverExpires);
    const cluster::TopologyPtr partial = partial_builder.build();
    Node lonely("n1", partial, &clock);
    expect_equal(lonely.run({"GET", "hello"}), "$-1\r\n",
                 "slot 866 is covered, so the command executes");
    expect_equal(lonely.run({"GET", "foo"}), "-CLUSTERDOWN Hash slot not served\r\n",
                 "an uncovered slot is CLUSTERDOWN, not MOVED");
    expect_contains(lonely.run({"CLUSTER", "INFO"}), "cluster_state:fail",
                    "partial slot coverage is not a healthy cluster");

    // ---------------------------------------------------------------------
    // Topology updates swap an immutable snapshot.
    // ---------------------------------------------------------------------
    Node failover("n1", stable, &clock);
    expect_equal(failover.run({"GET", "hello"}), "$-1\r\n", "n1 currently leads shard 1");
    failover.router().publishTopology(baseCluster()
                                          .setLeaderHint(1, "n2", 5, kNeverExpires)
                                          .setLeaderHint(2, "n2", 9, kNeverExpires)
                                          .build());
    expect_equal(failover.run({"GET", "hello"}), "-MOVED 866 127.0.0.1:7002\r\n",
                 "after shard 1 elects n2, the old leader redirects there");

    // ---------------------------------------------------------------------
    // Topology validation rejects inconsistent descriptions.
    // ---------------------------------------------------------------------
    bool threw = false;
    try {
        cluster::TopologyBuilder bad;
        bad.addNode("n1", "127.0.0.1", 7001)
            .addShard(1, {"n1"})
            .addShard(2, {"n1"})
            .assignSlots(1, cluster::SlotRange{0, 100})
            .assignSlots(2, cluster::SlotRange{50, 200});
        bad.build();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "a slot cannot be claimed by two shards");

    threw = false;
    try {
        cluster::TopologyBuilder bad = baseCluster();
        bad.setLeaderHint(1, "n2", 1, kNeverExpires).setMigration(866, 2, 1, true);
        bad.build();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "a migration source must be the committed owner of the slot");

    // ---------------------------------------------------------------------
    // Config file: same snapshot shape, different source.
    // ---------------------------------------------------------------------
    const std::string config_text =
        "# three nodes, two shards\n"
        "self n2\n"
        "epoch 12\n"
        "node n1 127.0.0.1 7001\n"
        "node n2 127.0.0.1 7002\n"
        "node n3 127.0.0.1 7003\n"
        "shard 1 n1 n2 n3\n"
        "shard 2 n2 n3 n1\n"
        "auto-slots\n"
        "leader 1 n1 4\n"
        "leader 2 n2 9\n"
        "migrating 866 1 2 ready\n";

    const cluster::ClusterConfig parsed = cluster::parseClusterConfig(config_text, clock);
    expect_equal(parsed.self_id, "n2", "the config names the local node");
    expect(parsed.topology->configEpoch() == 12, "config epoch is carried through");
    expect(parsed.topology->raftGroupCount() == 2, "auto-slots keeps 2 Raft groups");
    expect(parsed.topology->allSlotsAssigned(), "auto-slots covers all 16384 slots");
    expect(parsed.topology->slotOwner(0) == 1 && parsed.topology->slotOwner(16383) == 2,
           "slots are split evenly in declaration order");
    expect(parsed.topology->migratingSlotCount() == 1, "the migration is recorded");

    // One topology description can be shared by every node in the cluster.
    const cluster::ClusterConfig overridden =
        cluster::parseClusterConfig(config_text, clock, "n3");
    expect_equal(overridden.self_id, "n3",
                 "an explicit self id overrides the file's self line");

    threw = false;
    try {
        cluster::parseClusterConfig("self n1\nnode n1 127.0.0.1 7001\n", clock);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "a config without any shard is rejected");

    // ---------------------------------------------------------------------
    // Single-node mode is unchanged.
    // ---------------------------------------------------------------------
    Database standalone_db(true, [&clock] { return clock; });
    CommandHandler standalone(standalone_db);
    expect(!standalone.clusterEnabled(), "no router means no cluster mode");
    expect_equal(standalone.execute(toArguments({"SET", "foo", "1"})), "+OK\r\n",
                 "any key can be written on a single node");
    expect_equal(standalone.execute(toArguments({"DEL", "foo", "bar"})), ":1\r\n",
                 "cross-slot key sets are fine without sharding");
    expect_equal(standalone.execute(toArguments({"KEYS", "*"})), "*0\r\n",
                 "KEYS stays available on a single node");
    expect_equal(standalone.execute(toArguments({"ASKING"})),
                 "-ERR This instance has cluster support disabled\r\n",
                 "ASKING is meaningless without cluster support");
    expect_equal(standalone.execute(toArguments({"CLUSTER", "SLOTS"})),
                 "-ERR This instance has cluster support disabled\r\n",
                 "so is CLUSTER SLOTS");
    expect_equal(standalone.execute(toArguments({"CLUSTER", "KEYSLOT", "foo"})),
                 ":12182\r\n", "CLUSTER KEYSLOT is a pure function and stays available");

    // Snapshot publication is lock-free for readers and monotonic by revision.
    cluster::TopologyBuilder revision8_builder = stableCluster();
    const cluster::TopologyPtr revision8 =
        revision8_builder.setTopologyRevision(8).build();
    std::atomic<bool> readers_ok{true};
    std::thread reader([&] {
        for (int iteration = 0; iteration < 5000; ++iteration) {
            const cluster::TopologyPtr snapshot = n1.router().topology();
            if (!snapshot ||
                (snapshot->topologyRevision() != 7 &&
                 snapshot->topologyRevision() != 8) ||
                snapshot->slotOwner(866) != 1 ||
                snapshot->slotOwner(12182) != 2) {
                readers_ok.store(false);
                return;
            }
        }
    });
    expect(n1.router().publishTopology(revision8),
           "newer topology revision publishes");
    expect(!n1.router().publishTopology(stable),
           "older topology revision cannot overwrite a newer snapshot");
    reader.join();
    expect(readers_ok.load() && n1.router().topology()->topologyRevision() == 8,
           "concurrent readers observe one complete immutable snapshot");

    std::cout << "ClusterRoutingTest passed" << std::endl;
    return EXIT_SUCCESS;
}
