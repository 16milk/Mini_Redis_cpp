// Command.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mini_redis/cluster/ClientSession.hpp"
#include "mini_redis/net/Protocol.hpp"

class Database;

namespace cluster {
class ClusterRouter;
struct RouteDecision;
} // namespace cluster

class CommandHandler {
public:
    // 单节点模式：router 为空，命令直接在本地执行。
    explicit CommandHandler(Database& db) : db_(db) {}
    // 集群模式：先经 router 做 slot / owner / 迁移状态判定，再决定本地执行或重定向。
    CommandHandler(Database& db, cluster::ClusterRouter* router)
        : db_(db), router_(router) {}

    bool clusterEnabled() const { return router_ != nullptr; }

    // 执行命令，返回 RESP 响应字符串。使用内部会话，适用于单节点与测试。
    std::string execute(const std::vector<std::string>& args);

    // ASKING 这类一次性路由状态属于连接，因此集群模式下需要显式传入会话。
    std::string execute(const std::vector<std::string>& args,
                        cluster::ClientSession& session);

    // Reactor 已经分配了 request_seq 时使用，避免二次编号。
    std::string executePrepared(const std::vector<std::string>& args,
                                cluster::ClientSession& session,
                                std::uint64_t request_seq);

    // Group Actor 串行执行入口：不再碰连接会话，只访问本分片状态机。
    std::string executeOnStore(const std::vector<std::string>& args);

    static std::string encodeRedirect(const cluster::RouteDecision& decision);

private:
    Database& db_;
    cluster::ClusterRouter* router_ = nullptr;
    cluster::ClientSession default_session_;

    // 命令名已大写，负责把请求送到具体处理函数。
    std::string dispatch(const std::string& command,
                         const std::vector<std::string>& args);

    // 具体命令处理函数
    std::string handlePing(const std::vector<std::string>& args);
    std::string handleSet(const std::vector<std::string>& args);
    std::string handleGet(const std::vector<std::string>& args);
    std::string handleExpire(const std::vector<std::string>& args, bool milliseconds);
    std::string handleTtl(const std::vector<std::string>& args, bool milliseconds);
    std::string handlePersist(const std::vector<std::string>& args);

    std::string handleHSet(const std::vector<std::string>& args);
    std::string handleHGet(const std::vector<std::string>& args);

    std::string handleLPush(const std::vector<std::string>& args);
    std::string handleRPush(const std::vector<std::string>& args);
    std::string handleLPop(const std::vector<std::string>& args);
    std::string handleRPop(const std::vector<std::string>& args);
    std::string handleLIndex(const std::vector<std::string>& args);
    std::string handleLRange(const std::vector<std::string>& args);
    std::string handleLLen(const std::vector<std::string>& args);
    std::string handleLRem(const std::vector<std::string>& args);
    std::string handleLTrim(const std::vector<std::string>& args);

    std::string handleSAdd(const std::vector<std::string>& args);
    std::string handleSRem(const std::vector<std::string>& args);
    std::string handleSIsMember(const std::vector<std::string>& args);
    std::string handleSMembers(const std::vector<std::string>& args);
    std::string handleSCard(const std::vector<std::string>& args);

    std::string handleZAdd(const std::vector<std::string>& args);
    std::string handleZRem(const std::vector<std::string>& args);
    std::string handleZScore(const std::vector<std::string>& args);
    std::string handleZRange(const std::vector<std::string>& args);
    std::string handleZRangeByScore(const std::vector<std::string>& args);
    std::string handleZRank(const std::vector<std::string>& args);
    std::string handleZCard(const std::vector<std::string>& args);

    std::string handleDel(const std::vector<std::string>& args);
    std::string handleExists(const std::vector<std::string>& args);
    std::string handleKeys(const std::vector<std::string>& args);

    std::string handleSave(const std::vector<std::string>& args);

    // --- 集群路由命令 ---
    std::string handleAsking(const std::vector<std::string>& args,
                             cluster::ClientSession& session,
                             std::uint64_t request_seq);
    std::string handleCluster(const std::vector<std::string>& args);
    std::string handleClusterSlots();
    std::string handleClusterInfo();
};
