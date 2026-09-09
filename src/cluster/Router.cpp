#include "mini_redis/cluster/Router.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace cluster {

ClusterRouter::ClusterRouter(NodeId self_id, TopologyPtr topology)
    : self_id_(std::move(self_id)), topology_(std::move(topology)) {}

TopologyPtr ClusterRouter::topology() const {
    return std::atomic_load_explicit(&topology_, std::memory_order_acquire);
}

bool ClusterRouter::publishTopology(TopologyPtr topology) {
    if (!topology) {
        return false;
    }
    TopologyPtr current =
        std::atomic_load_explicit(&topology_, std::memory_order_acquire);
    for (;;) {
        if (current &&
            topology->topologyRevision() < current->topologyRevision()) {
            return false;
        }
        if (std::atomic_compare_exchange_weak_explicit(
                &topology_, &current, topology, std::memory_order_release,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

RouteDecision ClusterRouter::route(const CommandSpec& spec,
                                   const std::vector<std::string>& args,
                                   const ClientSession& session,
                                   std::uint64_t request_seq,
                                   std::int64_t now_ms) const {
    RouteDecision decision;

    if (!spec.cluster_supported) {
        decision.action = RouteAction::kUnsupported;
        decision.detail = std::string(spec.name) +
                          " is not available in cluster mode";
        return decision;
    }
    if (spec.access == AccessMode::kLocal || spec.key_layout == KeyLayout::kNone) {
        return decision;  // kLocal：无 key 的命令可在任意健康节点执行
    }

    const std::vector<std::string_view> keys = extractRoutingKeys(spec, args);
    if (keys.empty()) {
        // 参数个数不足，无法提取 key。放行给命令自身返回既有的 arity 错误，
        // 避免路由层对同一种错误给出第二套文案。
        return decision;
    }
    return routeKeys(keys, session, request_seq, now_ms);
}

RouteDecision ClusterRouter::routeKeys(const std::vector<std::string_view>& keys,
                                       const ClientSession& session,
                                       std::uint64_t request_seq,
                                       std::int64_t now_ms) const {
    RouteDecision decision;
    if (keys.empty()) {
        return decision;
    }

    // 一次请求只允许落在一个一致性域内，因此先要求所有 key 同 slot。
    const SlotId slot = keyToSlot(keys.front());
    for (std::size_t index = 1; index < keys.size(); ++index) {
        if (keyToSlot(keys[index]) != slot) {
            decision.action = RouteAction::kCrossSlot;
            return decision;
        }
    }
    decision.has_slot = true;
    decision.slot = slot;

    const TopologyPtr snapshot = topology();
    if (!snapshot || snapshot->shardCount() == 0) {
        decision.action = RouteAction::kClusterDown;
        decision.detail = "Cluster topology is not initialized";
        return decision;
    }

    const ShardId owner = snapshot->slotOwner(slot);
    if (owner == kNoShard) {
        decision.action = RouteAction::kClusterDown;
        decision.detail = "Hash slot not served";
        return decision;
    }
    decision.shard = owner;
    decision.slot_epoch = snapshot->slotEpoch(slot);

    const SlotMigration* migration = snapshot->migrationForSlot(slot);
    // 读和写都要求本节点是该分片 Raft group 的 leader：写需要 leader 才能
    // propose，强一致读需要 leader 的 ReadIndex。follower 只做重定向。
    const std::optional<NodeId> owner_leader = snapshot->trustedLeader(owner, now_ms);
    const bool self_is_owner_leader = owner_leader.has_value() &&
                                      *owner_leader == self_id_;

    if (self_is_owner_leader) {
        // 迁移中的源分片：只有目标已经安全激活（源 Raft 已提交
        // TargetReady(proof)）才打开 ASK 窗口。
        //
        // 一旦打开就是全量转发，不再按 key 是否还在本地判断。源端此时已经
        // Fence：它手里的那份数据是快照，目标端却已经在接受 ASKING 写入。
        // 只要本地还留着 key 就本地作答，等于把已经过时的值当成当前值返回，
        // 破坏线性一致性。Fence 之后源端对这个 slot 不再有任何发言权，
        // 键在不在本地都一样。
        if (migration != nullptr && migration->target_ready) {
            const std::optional<NodeId> target_leader =
                snapshot->trustedLeader(migration->target, now_ms);
            if (target_leader && *target_leader == self_id_) {
                // 同一节点同时是源分片和目标分片的 leader。重定向到自己
                // 只会让客户端在 ASK 和本地之间打转，直接本地执行。
                decision.action = RouteAction::kLocal;
                decision.shard = migration->target;
                decision.slot_epoch = migration->to_epoch;
                decision.consumed_asking = true;
                return decision;
            }
            const NodeRecord* target_node =
                target_leader ? snapshot->findNode(*target_leader) : nullptr;
            if (target_node == nullptr) {
                decision.action = RouteAction::kTryAgain;
                decision.detail = "Migration target leader unknown";
                return decision;
            }
            decision.action = RouteAction::kAsk;
            decision.shard = migration->target;
            decision.endpoint = target_node->client;
            return decision;
        }
        decision.action = RouteAction::kLocal;
        return decision;
    }

    // 迁移中的目标分片：只有本地 Raft 已提交对应的迁移状态时，
    // 才消费客户端的一次性 ASKING 标志。单独发 ASKING 拿不到任何访问权。
    if (migration != nullptr && migration->target_ready) {
        const std::optional<NodeId> target_leader =
            snapshot->trustedLeader(migration->target, now_ms);
        if (target_leader && *target_leader == self_id_ &&
            session.askingFor(request_seq)) {
            // 目标端一旦 target_ready，它持有的就是 Fence 时刻的完整 slot，
            // 不存在“搬了一半”的中间态。此处不再探测 key 是否存在：一个
            // 本来就不存在的 key 会被误判成尚未搬到，把请求永远推进 TRYAGAIN。
            decision.shard = migration->target;
            decision.slot_epoch = migration->to_epoch;
            decision.consumed_asking = true;
            decision.action = RouteAction::kLocal;
            return decision;
        }
    }

    if (!owner_leader) {
        // leader 未知时不能猜一个地址，否则两个节点可能互相指向形成环。
        decision.action = RouteAction::kTryAgain;
        decision.detail = "Slot leader unknown, retry after topology refresh";
        return decision;
    }

    const NodeRecord* owner_node = snapshot->findNode(*owner_leader);
    if (owner_node == nullptr) {
        decision.action = RouteAction::kTryAgain;
        decision.detail = "Slot leader address unknown";
        return decision;
    }
    decision.action = RouteAction::kMoved;
    decision.endpoint = owner_node->client;
    return decision;
}

ClusterSlotsView ClusterRouter::clusterSlotsView(std::int64_t now_ms) const {
    ClusterSlotsView view;
    const TopologyPtr snapshot = topology();
    if (!snapshot || snapshot->shardCount() == 0) {
        view.unavailable_reason = "Cluster topology is not initialized";
        return view;
    }

    for (const auto& [range, shard_id] : snapshot->slotRanges()) {
        const std::optional<NodeId> leader = snapshot->trustedLeader(shard_id, now_ms);
        if (!leader) {
            // 半张 slot 表会让客户端缓存出错误路由，因此整条命令改为 TRYAGAIN。
            view.ranges.clear();
            view.unavailable_reason = "Leader unknown for shard " +
                                      std::to_string(shard_id);
            return view;
        }
        const NodeRecord* primary = snapshot->findNode(*leader);
        const ShardRecord* shard = snapshot->findShard(shard_id);
        if (primary == nullptr || shard == nullptr) {
            view.ranges.clear();
            view.unavailable_reason = "Incomplete node record for shard " +
                                      std::to_string(shard_id);
            return view;
        }

        SlotRangeView entry;
        entry.range = range;
        entry.shard = shard_id;
        entry.primary = *primary;
        for (const NodeId& voter : shard->voters) {
            if (voter == primary->id) {
                continue;
            }
            if (const NodeRecord* replica = snapshot->findNode(voter)) {
                entry.replicas.push_back(*replica);
            }
        }
        view.ranges.push_back(std::move(entry));
    }

    view.complete = true;
    return view;
}

ClusterStatusView ClusterRouter::statusView(std::int64_t now_ms) const {
    ClusterStatusView view;
    const TopologyPtr snapshot = topology();
    if (!snapshot) {
        return view;
    }

    view.slots_assigned = snapshot->assignedSlotCount();
    view.known_nodes = snapshot->nodeCount();
    view.shard_count = snapshot->shardCount();
    view.config_epoch = snapshot->configEpoch();
    view.local_shards = snapshot->shardsHostedBy(self_id_);

    bool every_shard_has_leader = snapshot->shardCount() > 0;
    for (const ShardRecord& shard : snapshot->shards()) {
        const std::optional<NodeId> leader = snapshot->trustedLeader(shard.id, now_ms);
        if (!leader) {
            every_shard_has_leader = false;
            continue;
        }
        if (*leader == self_id_) {
            ++view.local_leader_shards;
        }
    }
    view.migrating_slots = snapshot->migratingSlotCount();
    view.state_ok = snapshot->allSlotsAssigned() && every_shard_has_leader;
    return view;
}

} // namespace cluster
