#pragma once

#include "mini_redis/cluster/ClientSession.hpp"
#include "mini_redis/cluster/Topology.hpp"
#include "mini_redis/consensus/RaftTypes.hpp"
#include "mini_redis/runtime/CompletionQueue.hpp"
#include "mini_redis/runtime/GroupScheduler.hpp"
#include "mini_redis/runtime/IoBudget.hpp"
#include "mini_redis/runtime/PipelineReorder.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class CommandHandler;

namespace cluster {
class ClusterRouter;
}

namespace runtime {

constexpr consensus::GroupId kStandaloneGroupId = 1;

enum class FrontendAction {
    kImmediate,  // response is ready now (PING, redirect, local error)
    kDeferred,   // submitted to a group actor
    kHold,       // leave the command in the input buffer and pause reads
};

struct FrontendResult {
    FrontendAction action = FrontendAction::kImmediate;
    std::string response;
    bool pause_reads = false;
};

struct RequestContext {
    cluster::ClientSession& session;
    PipelineReorder& pipeline;
    CompletionQueue& completions;
    std::uint64_t connection_id = 0;
    std::uint64_t generation = 0;
    TrafficClass traffic = TrafficClass::kClient;
};

// Reactor-side frontend: parse-time routing, local session commands, and
// submission of data commands onto the owning group actor.
class RequestFrontend {
public:
    RequestFrontend(GroupScheduler& scheduler, CommandHandler& local_commands,
                    cluster::ClusterRouter* router = nullptr);

    using GroupResolver =
        std::function<consensus::GroupId(const std::vector<std::string>&)>;
    void setGroupResolver(GroupResolver resolver) { resolver_ = std::move(resolver); }

    FrontendResult handle(RequestContext& ctx, const std::vector<std::string>& args);
    bool shouldPauseReads(const RequestContext& ctx) const;
    bool groupAccepts(consensus::GroupId id) const;

private:
    consensus::GroupId resolveGroup(const std::vector<std::string>& args,
                                    cluster::ShardId shard) const;
    FrontendResult finishImmediate(RequestContext& ctx, std::uint64_t seq,
                                   std::string response, bool pause_reads);
    FrontendResult tryAgain(RequestContext& ctx, std::uint64_t seq,
                            const std::string& detail);

    GroupScheduler& scheduler_;
    CommandHandler& local_commands_;
    cluster::ClusterRouter* router_ = nullptr;
    GroupResolver resolver_;
};

}  // namespace runtime
