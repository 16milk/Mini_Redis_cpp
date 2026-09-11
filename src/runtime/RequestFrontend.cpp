#include "mini_redis/runtime/RequestFrontend.hpp"

#include "mini_redis/cluster/Router.hpp"
#include "mini_redis/command/Command.hpp"
#include "mini_redis/command/CommandSpec.hpp"
#include "mini_redis/net/Protocol.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace runtime {

namespace {

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool isReactorLocal(const std::string& cmd, const CommandSpec* spec) {
    if (cmd == "ASKING" || cmd == "CLUSTER" || cmd == "PING") {
        return true;
    }
    return spec != nullptr && spec->access == AccessMode::kLocal;
}

class AskingGuard {
public:
    AskingGuard(cluster::ClientSession& session, std::uint64_t request_seq)
        : session_(session), request_seq_(request_seq) {}
    ~AskingGuard() {
        if (session_.askingFor(request_seq_)) {
            session_.clearAsking();
        }
    }
    AskingGuard(const AskingGuard&) = delete;
    AskingGuard& operator=(const AskingGuard&) = delete;

private:
    cluster::ClientSession& session_;
    std::uint64_t request_seq_;
};

}  // namespace

RequestFrontend::RequestFrontend(GroupScheduler& scheduler,
                                 CommandHandler& local_commands,
                                 cluster::ClusterRouter* router)
    : scheduler_(scheduler), local_commands_(local_commands), router_(router) {}

FrontendResult RequestFrontend::handle(RequestContext& ctx,
                                       const std::vector<std::string>& args) {
    if (args.empty()) {
        const std::uint64_t seq = ctx.session.beginRequest();
        if (!ctx.pipeline.begin(seq)) {
            return tryAgain(ctx, seq, "too many pending requests");
        }
        return finishImmediate(ctx, seq, RespParser::encodeError("empty command"),
                               false);
    }

    if (ctx.pipeline.pauseReads() || !ctx.pipeline.canBegin()) {
        FrontendResult result;
        result.action = FrontendAction::kHold;
        result.pause_reads = true;
        return result;
    }

    const std::string cmd = toUpper(args[0]);
    const CommandSpec* spec = lookupCommandSpec(cmd);
    const std::uint64_t seq = ctx.session.beginRequest();
    AskingGuard asking_guard(ctx.session, seq);
    if (!ctx.pipeline.begin(seq)) {
        return tryAgain(ctx, seq, "too many pending requests");
    }

    if (isReactorLocal(cmd, spec) || spec == nullptr) {
        return finishImmediate(ctx, seq,
                               local_commands_.executePrepared(args, ctx.session, seq),
                               false);
    }

    cluster::RouteDecision decision;
    if (router_ != nullptr && spec != nullptr) {
        decision = router_->route(*spec, args, ctx.session, seq, nowMs());
        if (decision.action != cluster::RouteAction::kLocal) {
            return finishImmediate(ctx, seq, CommandHandler::encodeRedirect(decision),
                                   false);
        }
    }

    const consensus::GroupId group_id = resolveGroup(args, decision.shard);
    if (!scheduler_.hasGroup(group_id) || !scheduler_.canAcceptClient(group_id)) {
        return tryAgain(ctx, seq, "busy");
    }

    ClientWork work;
    work.connection_id = ctx.connection_id;
    work.generation = ctx.generation;
    work.request_seq = seq;
    work.args = args;
    work.reply_to = &ctx.completions;
    if (!scheduler_.submitClient(group_id, std::move(work))) {
        return tryAgain(ctx, seq, "busy");
    }

    FrontendResult result;
    result.action = FrontendAction::kDeferred;
    result.pause_reads = ctx.pipeline.pauseReads() || !scheduler_.clientBelowResume(group_id);
    return result;
}

bool RequestFrontend::shouldPauseReads(const RequestContext& ctx) const {
    if (ctx.pipeline.pauseReads()) {
        return true;
    }
    if (router_ != nullptr) {
        return false;
    }
    return scheduler_.hasGroup(kStandaloneGroupId) &&
           !scheduler_.canAcceptClient(kStandaloneGroupId);
}

bool RequestFrontend::groupAccepts(consensus::GroupId id) const {
    return scheduler_.canAcceptClient(id);
}

consensus::GroupId RequestFrontend::resolveGroup(const std::vector<std::string>& args,
                                                 cluster::ShardId shard) const {
    if (resolver_) {
        return resolver_(args);
    }
    if (shard != cluster::kNoShard) {
        return static_cast<consensus::GroupId>(shard);
    }
    return kStandaloneGroupId;
}

FrontendResult RequestFrontend::finishImmediate(RequestContext& ctx, std::uint64_t seq,
                                                std::string response, bool pause_reads) {
    ctx.pipeline.complete(seq, std::move(response));
    FrontendResult result;
    result.action = FrontendAction::kImmediate;
    result.pause_reads = pause_reads || ctx.pipeline.pauseReads();
    return result;
}

FrontendResult RequestFrontend::tryAgain(RequestContext& ctx, std::uint64_t seq,
                                         const std::string& detail) {
    return finishImmediate(ctx, seq, RespParser::encodeTryAgainError(detail), true);
}

}  // namespace runtime
