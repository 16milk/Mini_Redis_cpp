#pragma once

#include "mini_redis/cluster/ClientSession.hpp"
#include "mini_redis/cluster/Topology.hpp"
#include "mini_redis/command/CommandSpec.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cluster {

enum class RouteAction {
    kLocal,        // 本节点是该 slot 的可信 leader，正常执行
    kMoved,        // 本节点不是 owner，客户端应更新缓存并改访问目标节点
    kAsk,          // 迁移切换窗口内的一次性重定向
    kCrossSlot,    // 多 key 命令跨越了不同 slot
    kTryAgain,     // 拓扑暂时无法判定
    kClusterDown,  // slot 未被任何分片覆盖
    kUnsupported,  // 该命令在 cluster 模式下不提供
};

struct RouteDecision {
    RouteAction action = RouteAction::kLocal;
    bool has_slot = false;
    SlotId slot = 0;
    ShardId shard = kNoShard;
    std::uint64_t slot_epoch = 0;
    Endpoint endpoint;  // 仅 kMoved / kAsk 有意义
    std::string detail;
    bool consumed_asking = false;
};

// 迁移是整槽原子切换：Fence 之前整个 slot 都归源端，Fence 之后整个 slot 都归
// 目标端，不存在 Redis Cluster 那种逐 key 搬迁的中间态。因此路由不再探测 key
// 是否存在于本地——那个信号在这个协议里既不必要（切换是全量的），也不安全
// （源端 Fence 后仍持有过期副本，按存在与否作答会返回陈旧值）。

// CLUSTER SLOTS 的结构化结果。RESP 编码留在最外层，不进入路由逻辑。
struct SlotRangeView {
    SlotRange range;
    ShardId shard = kNoShard;
    NodeRecord primary;                // 当前可信 leader
    std::vector<NodeRecord> replicas;  // 该 Raft group 的其余 voter
};

struct ClusterSlotsView {
    bool complete = false;  // false 时调用方应返回 TRYAGAIN，而不是半张 slot 表
    std::string unavailable_reason;
    std::vector<SlotRangeView> ranges;
};

struct ClusterStatusView {
    bool state_ok = false;
    std::size_t slots_assigned = 0;
    std::size_t known_nodes = 0;
    std::size_t shard_count = 0;  // 等于 Raft group 数量
    std::uint64_t config_epoch = 0;
    std::size_t migrating_slots = 0;
    std::vector<ShardId> local_shards;
    std::size_t local_leader_shards = 0;
};

class ClusterRouter {
public:
    ClusterRouter(NodeId self_id, TopologyPtr topology);

    const NodeId& selfId() const { return self_id_; }
    TopologyPtr topology() const;

    // 发布新的不可变快照。已在路由中的请求继续使用旧快照。
    // Returns false when an older revision tried to replace a newer snapshot.
    bool publishTopology(TopologyPtr topology);

    RouteDecision route(const CommandSpec& spec, const std::vector<std::string>& args,
                        const ClientSession& session, std::uint64_t request_seq,
                        std::int64_t now_ms) const;

    RouteDecision routeKeys(const std::vector<std::string_view>& keys,
                            const ClientSession& session, std::uint64_t request_seq,
                            std::int64_t now_ms) const;

    ClusterSlotsView clusterSlotsView(std::int64_t now_ms) const;
    ClusterStatusView statusView(std::int64_t now_ms) const;

private:
    NodeId self_id_;
    TopologyPtr topology_;
};

} // namespace cluster
