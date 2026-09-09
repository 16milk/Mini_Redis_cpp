// Command.cpp
#include "mini_redis/command/Command.hpp"
#include "mini_redis/cluster/Router.hpp"
#include "mini_redis/command/CommandSpec.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

bool parseInteger(const std::string& value, long long& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno == ERANGE || end != value.c_str() + value.size()) return false;
    out = parsed;
    return true;
}

bool parseExpiration(const std::string& value, UnixMillis& out) {
    if (value.empty()) return false;

    std::size_t digit_index = 0;
    if (value.front() == '-') {
        digit_index = 1;
    }
    if (digit_index == value.size()) return false;
    if (value[digit_index] == '0' && value.size() - digit_index != 1) return false;
    if (digit_index == 1 && value[digit_index] == '0') return false;
    for (; digit_index < value.size(); ++digit_index) {
        const unsigned char character = static_cast<unsigned char>(value[digit_index]);
        if (!std::isdigit(character)) return false;
    }

    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno == ERANGE || end != value.c_str() + value.size() ||
        parsed < std::numeric_limits<UnixMillis>::min() ||
        parsed > std::numeric_limits<UnixMillis>::max()) {
        return false;
    }
    out = static_cast<UnixMillis>(parsed);
    return true;
}

bool secondsToMilliseconds(UnixMillis seconds, UnixMillis& milliseconds) {
    constexpr UnixMillis kMillisecondsPerSecond = 1000;
    if (seconds > std::numeric_limits<UnixMillis>::max() / kMillisecondsPerSecond ||
        seconds < std::numeric_limits<UnixMillis>::min() / kMillisecondsPerSecond) {
        return false;
    }
    milliseconds = seconds * kMillisecondsPerSecond;
    return true;
}

bool parseScore(const std::string& value, double& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno == ERANGE || end != value.c_str() + value.size() || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

bool parseScoreBoundary(const std::string& value, double& out) {
    if (value == "+inf" || value == "+INF") {
        out = std::numeric_limits<double>::infinity();
        return true;
    }
    if (value == "-inf" || value == "-INF") {
        out = -std::numeric_limits<double>::infinity();
        return true;
    }
    return parseScore(value, out);
}

std::string typeError(const std::exception& exception) {
    (void)exception;
    return RespParser::encodeWrongTypeError();
}

// ASKING 是一次性标志：无论紧随其后的那条命令成功、失败还是没有 key，
// 都必须把它消费掉。标志绑定在请求序号上，所以 ASKING 自身不会清除
// 它刚刚为下一个序号设置的状态。
class AskingConsumer {
public:
    AskingConsumer(cluster::ClientSession& session, std::uint64_t request_seq)
        : session_(session), request_seq_(request_seq) {}
    ~AskingConsumer() {
        if (session_.askingFor(request_seq_)) {
            session_.clearAsking();
        }
    }
    AskingConsumer(const AskingConsumer&) = delete;
    AskingConsumer& operator=(const AskingConsumer&) = delete;

private:
    cluster::ClientSession& session_;
    std::uint64_t request_seq_;
};

// CLUSTER SLOTS 的 node tuple 固定为 [host, port, node-id]。
std::string encodeClusterNode(const cluster::NodeRecord& node) {
    std::vector<std::string> fields;
    fields.push_back(RespParser::encodeBulkString(node.client.host));
    fields.push_back(RespParser::encodeInteger(node.client.port));
    fields.push_back(RespParser::encodeBulkString(node.id));
    return RespParser::encodeArrayOfEncoded(fields);
}

} // namespace

static std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string CommandHandler::execute(const std::vector<std::string>& args) {
    return execute(args, default_session_);
}

std::string CommandHandler::execute(const std::vector<std::string>& args,
                                    cluster::ClientSession& session) {
    const std::uint64_t request_seq = session.beginRequest();
    const AskingConsumer asking_consumer(session, request_seq);

    if (args.empty()) {
        return RespParser::encodeError("empty command");
    }

    const std::string cmd = toUpper(args[0]);

    // 路由控制命令自身不参与 slot 路由，也不进入数据 Raft。
    if (cmd == "ASKING") {
        return handleAsking(args, session, request_seq);
    }
    if (cmd == "CLUSTER") {
        return handleCluster(args);
    }

    if (router_ == nullptr) {
        return dispatch(cmd, args);
    }

    const CommandSpec* spec = lookupCommandSpec(cmd);
    if (spec == nullptr) {
        return RespParser::encodeError("unknown command `" + args[0] + "`");
    }

    const cluster::RouteDecision decision =
        router_->route(*spec, args, session, request_seq, db_.nowMs());
    if (decision.action != cluster::RouteAction::kLocal) {
        return encodeRedirect(decision);
    }
    return dispatch(cmd, args);
}

std::string CommandHandler::encodeRedirect(const cluster::RouteDecision& decision) {
    switch (decision.action) {
        case cluster::RouteAction::kMoved:
            // 客户端应更新自己的 slot 缓存并重发到新地址。
            return RespParser::encodeMovedError(decision.slot,
                                                decision.endpoint.toRedirectTarget());
        case cluster::RouteAction::kAsk:
            // 一次性重定向：客户端先发 ASKING，再把同一条命令发往目标节点。
            return RespParser::encodeAskError(decision.slot,
                                              decision.endpoint.toRedirectTarget());
        case cluster::RouteAction::kCrossSlot:
            return RespParser::encodeCrossSlotError();
        case cluster::RouteAction::kTryAgain:
            return RespParser::encodeTryAgainError(decision.detail);
        case cluster::RouteAction::kClusterDown:
            return RespParser::encodeClusterDownError(decision.detail);
        case cluster::RouteAction::kUnsupported:
            return RespParser::encodeError(decision.detail);
        case cluster::RouteAction::kLocal:
            break;
    }
    return RespParser::encodeError("internal routing error");
}

std::string CommandHandler::dispatch(const std::string& cmd,
                                     const std::vector<std::string>& args) {
    if (cmd == "PING") {
        return handlePing(args);
    } else if (cmd == "SET") {
        return handleSet(args);
    } else if (cmd == "GET") {
        return handleGet(args);
    } else if (cmd == "EXPIRE") {
        return handleExpire(args, false);
    } else if (cmd == "PEXPIRE") {
        return handleExpire(args, true);
    } else if (cmd == "TTL") {
        return handleTtl(args, false);
    } else if (cmd == "PTTL") {
        return handleTtl(args, true);
    } else if (cmd == "PERSIST") {
        return handlePersist(args);
    } else if (cmd == "HSET") {
        return handleHSet(args);
    } else if (cmd == "HGET") {
        return handleHGet(args);
    } else if (cmd == "LPUSH") {
        return handleLPush(args);
    } else if (cmd == "RPUSH") {
        return handleRPush(args);
    } else if (cmd == "LPOP") {
        return handleLPop(args);
    } else if (cmd == "RPOP") {
        return handleRPop(args);
    } else if (cmd == "LINDEX") {
        return handleLIndex(args);
    } else if (cmd == "LRANGE") {
        return handleLRange(args);
    } else if (cmd == "LLEN") {
        return handleLLen(args);
    } else if (cmd == "LREM") {
        return handleLRem(args);
    } else if (cmd == "LTRIM") {
        return handleLTrim(args);
    } else if (cmd == "SADD") {
        return handleSAdd(args);
    } else if (cmd == "SREM") {
        return handleSRem(args);
    } else if (cmd == "SISMEMBER") {
        return handleSIsMember(args);
    } else if (cmd == "SMEMBERS") {
        return handleSMembers(args);
    } else if (cmd == "SCARD") {
        return handleSCard(args);
    } else if (cmd == "ZADD") {
        return handleZAdd(args);
    } else if (cmd == "ZREM") {
        return handleZRem(args);
    } else if (cmd == "ZSCORE") {
        return handleZScore(args);
    } else if (cmd == "ZRANGE") {
        return handleZRange(args);
    } else if (cmd == "ZRANGEBYSCORE") {
        return handleZRangeByScore(args);
    } else if (cmd == "ZRANK") {
        return handleZRank(args);
    } else if (cmd == "ZCARD") {
        return handleZCard(args);
    } else if (cmd == "DEL") {
        return handleDel(args);
    } else if (cmd == "EXISTS") {
        return handleExists(args);
    } else if (cmd == "KEYS") {
        return handleKeys(args);
    } else if (cmd == "SAVE") {
        return handleSave(args);
    } else {
        return RespParser::encodeError("unknown command `" + args[0] + "`");
    }
}

std::string CommandHandler::handlePing(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        return RespParser::encodeSimpleString("PONG");
    }
    if (args.size() == 2) {
        return RespParser::encodeBulkString(args[1]);
    }
    return RespParser::encodeError("wrong number of arguments for 'PING'");
}

std::string CommandHandler::handleSet(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        return RespParser::encodeError("wrong number of arguments for 'SET'");
    }
    if (args.size() == 3) {
        db_.set(args[1], args[2]);
        return RespParser::encodeSimpleString("OK");
    }
    if (args.size() != 5) {
        return RespParser::encodeError("syntax error");
    }

    const std::string option = toUpper(args[3]);
    if (option != "EX" && option != "PX") {
        return RespParser::encodeError("syntax error");
    }

    UnixMillis timeout;
    if (!parseExpiration(args[4], timeout)) {
        return RespParser::encodeError("value is not an integer or out of range");
    }
    if (timeout <= 0) {
        return RespParser::encodeError("invalid expire time in 'set' command");
    }

    UnixMillis ttl_ms = timeout;
    if (option == "EX" && !secondsToMilliseconds(timeout, ttl_ms)) {
        return RespParser::encodeError("invalid expire time in 'set' command");
    }

    try {
        db_.set(args[1], args[2], ttl_ms);
    } catch (const std::invalid_argument&) {
        return RespParser::encodeError("invalid expire time in 'set' command");
    } catch (const std::overflow_error&) {
        return RespParser::encodeError("invalid expire time in 'set' command");
    }
    return RespParser::encodeSimpleString("OK");
}

std::string CommandHandler::handleGet(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return RespParser::encodeError("wrong number of arguments for 'GET'");
    }
    try {
        std::string value;
        return db_.get(args[1], value) ? RespParser::encodeBulkString(value)
                                       : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleExpire(const std::vector<std::string>& args,
                                         bool milliseconds) {
    const std::string command = milliseconds ? "PEXPIRE" : "EXPIRE";
    const std::string error_command = milliseconds ? "pexpire" : "expire";
    if (args.size() != 3) {
        return RespParser::encodeError("wrong number of arguments for '" + command + "'");
    }

    UnixMillis timeout;
    if (!parseExpiration(args[2], timeout)) {
        return RespParser::encodeError("value is not an integer or out of range");
    }

    UnixMillis ttl_ms = timeout;
    if (!milliseconds && !secondsToMilliseconds(timeout, ttl_ms)) {
        return RespParser::encodeError(
            "invalid expire time in '" + error_command + "' command");
    }

    try {
        return RespParser::encodeInteger(db_.expire(args[1], ttl_ms) ? 1 : 0);
    } catch (const std::overflow_error&) {
        return RespParser::encodeError(
            "invalid expire time in '" + error_command + "' command");
    }
}

std::string CommandHandler::handleTtl(const std::vector<std::string>& args,
                                      bool milliseconds) {
    const std::string command = milliseconds ? "PTTL" : "TTL";
    if (args.size() != 2) {
        return RespParser::encodeError("wrong number of arguments for '" + command + "'");
    }
    return RespParser::encodeInteger(db_.ttl(args[1], milliseconds));
}

std::string CommandHandler::handlePersist(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return RespParser::encodeError("wrong number of arguments for 'PERSIST'");
    }
    return RespParser::encodeInteger(db_.persist(args[1]) ? 1 : 0);
}

std::string CommandHandler::handleHSet(const std::vector<std::string>& args) {
    if (args.size() < 4 || (args.size() - 2) % 2 != 0) {
        return RespParser::encodeError("wrong number of arguments for 'HSET'");
    }
    try {
        std::vector<std::pair<std::string, std::string>> field_values;
        field_values.reserve((args.size() - 2) / 2);
        for (size_t index = 2; index < args.size(); index += 2) {
            field_values.emplace_back(args[index], args[index + 1]);
        }
        return RespParser::encodeInteger(static_cast<long long>(db_.hset(args[1], field_values)));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleHGet(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        return RespParser::encodeError("wrong number of arguments for 'HGET'");
    }
    try {
        std::string value;
        return db_.hget(args[1], args[2], value) ? RespParser::encodeBulkString(value)
                                                  : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLPush(const std::vector<std::string>& args) {
    if (args.size() < 3) return RespParser::encodeError("wrong number of arguments for 'LPUSH'");
    try {
        return RespParser::encodeInteger(static_cast<long long>(
            db_.lpush(args[1], std::vector<std::string>(args.begin() + 2, args.end()))));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleRPush(const std::vector<std::string>& args) {
    if (args.size() < 3) return RespParser::encodeError("wrong number of arguments for 'RPUSH'");
    try {
        return RespParser::encodeInteger(static_cast<long long>(
            db_.rpush(args[1], std::vector<std::string>(args.begin() + 2, args.end()))));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLPop(const std::vector<std::string>& args) {
    if (args.size() != 2) return RespParser::encodeError("wrong number of arguments for 'LPOP'");
    try {
        std::string value;
        return db_.lpop(args[1], value) ? RespParser::encodeBulkString(value)
                                        : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleRPop(const std::vector<std::string>& args) {
    if (args.size() != 2) return RespParser::encodeError("wrong number of arguments for 'RPOP'");
    try {
        std::string value;
        return db_.rpop(args[1], value) ? RespParser::encodeBulkString(value)
                                        : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLIndex(const std::vector<std::string>& args) {
    if (args.size() != 3) return RespParser::encodeError("wrong number of arguments for 'LINDEX'");
    long long index;
    if (!parseInteger(args[2], index)) return RespParser::encodeError("value is not an integer or out of range");
    try {
        std::string value;
        return db_.lindex(args[1], index, value) ? RespParser::encodeBulkString(value)
                                                  : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLRange(const std::vector<std::string>& args) {
    if (args.size() != 4) return RespParser::encodeError("wrong number of arguments for 'LRANGE'");
    long long start, stop;
    if (!parseInteger(args[2], start) || !parseInteger(args[3], stop)) {
        return RespParser::encodeError("value is not an integer or out of range");
    }
    try { return RespParser::encodeArray(db_.lrange(args[1], start, stop)); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLLen(const std::vector<std::string>& args) {
    if (args.size() != 2) return RespParser::encodeError("wrong number of arguments for 'LLEN'");
    try { return RespParser::encodeInteger(static_cast<long long>(db_.llen(args[1]))); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLRem(const std::vector<std::string>& args) {
    if (args.size() != 4) return RespParser::encodeError("wrong number of arguments for 'LREM'");
    long long count;
    if (!parseInteger(args[2], count)) return RespParser::encodeError("value is not an integer or out of range");
    try { return RespParser::encodeInteger(db_.lrem(args[1], count, args[3])); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleLTrim(const std::vector<std::string>& args) {
    if (args.size() != 4) return RespParser::encodeError("wrong number of arguments for 'LTRIM'");
    long long start, stop;
    if (!parseInteger(args[2], start) || !parseInteger(args[3], stop)) {
        return RespParser::encodeError("value is not an integer or out of range");
    }
    try {
        db_.ltrim(args[1], start, stop);
        return RespParser::encodeSimpleString("OK");
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleSAdd(const std::vector<std::string>& args) {
    if (args.size() < 3) return RespParser::encodeError("wrong number of arguments for 'SADD'");
    try {
        return RespParser::encodeInteger(static_cast<long long>(
            db_.sadd(args[1], std::vector<std::string>(args.begin() + 2, args.end()))));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleSRem(const std::vector<std::string>& args) {
    if (args.size() < 3) return RespParser::encodeError("wrong number of arguments for 'SREM'");
    try {
        return RespParser::encodeInteger(static_cast<long long>(
            db_.srem(args[1], std::vector<std::string>(args.begin() + 2, args.end()))));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleSIsMember(const std::vector<std::string>& args) {
    if (args.size() != 3) return RespParser::encodeError("wrong number of arguments for 'SISMEMBER'");
    try { return RespParser::encodeInteger(db_.sismember(args[1], args[2]) ? 1 : 0); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleSMembers(const std::vector<std::string>& args) {
    if (args.size() != 2) return RespParser::encodeError("wrong number of arguments for 'SMEMBERS'");
    try { return RespParser::encodeArray(db_.smembers(args[1])); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleSCard(const std::vector<std::string>& args) {
    if (args.size() != 2) return RespParser::encodeError("wrong number of arguments for 'SCARD'");
    try { return RespParser::encodeInteger(static_cast<long long>(db_.scard(args[1]))); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZAdd(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() % 2 != 0) {
        return RespParser::encodeError("wrong number of arguments for 'ZADD'");
    }
    std::vector<std::pair<double, std::string>> score_members;
    score_members.reserve((args.size() - 2) / 2);
    for (size_t index = 2; index < args.size(); index += 2) {
        double score;
        if (!parseScore(args[index], score)) return RespParser::encodeError("value is not a valid float");
        score_members.emplace_back(score, args[index + 1]);
    }
    try {
        return RespParser::encodeInteger(static_cast<long long>(db_.zadd(args[1], score_members)));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZRem(const std::vector<std::string>& args) {
    if (args.size() < 3) return RespParser::encodeError("wrong number of arguments for 'ZREM'");
    try {
        return RespParser::encodeInteger(static_cast<long long>(
            db_.zrem(args[1], std::vector<std::string>(args.begin() + 2, args.end()))));
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZScore(const std::vector<std::string>& args) {
    if (args.size() != 3) return RespParser::encodeError("wrong number of arguments for 'ZSCORE'");
    try {
        double score;
        return db_.zscore(args[1], args[2], score) ? RespParser::encodeBulkString(ZSetObject::format_score(score))
                                                   : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZRange(const std::vector<std::string>& args) {
    if (args.size() != 4 && args.size() != 5) {
        return RespParser::encodeError("wrong number of arguments for 'ZRANGE'");
    }
    long long start, stop;
    if (!parseInteger(args[2], start) || !parseInteger(args[3], stop)) {
        return RespParser::encodeError("value is not an integer or out of range");
    }
    if (args.size() == 5 && toUpper(args[4]) != "WITHSCORES") {
        return RespParser::encodeError("syntax error");
    }
    try { return RespParser::encodeArray(db_.zrange(args[1], start, stop, args.size() == 5)); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZRangeByScore(const std::vector<std::string>& args) {
    if (args.size() != 4 && args.size() != 5) {
        return RespParser::encodeError("wrong number of arguments for 'ZRANGEBYSCORE'");
    }
    double min, max;
    if (!parseScoreBoundary(args[2], min) || !parseScoreBoundary(args[3], max)) {
        return RespParser::encodeError("min or max is not a float");
    }
    if (args.size() == 5 && toUpper(args[4]) != "WITHSCORES") {
        return RespParser::encodeError("syntax error");
    }
    try { return RespParser::encodeArray(db_.zrangebyscore(args[1], min, max, args.size() == 5)); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZRank(const std::vector<std::string>& args) {
    if (args.size() != 3) return RespParser::encodeError("wrong number of arguments for 'ZRANK'");
    try {
        size_t rank;
        return db_.zrank(args[1], args[2], rank)
                   ? RespParser::encodeInteger(static_cast<long long>(rank))
                   : RespParser::encodeNullBulkString();
    } catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleZCard(const std::vector<std::string>& args) {
    if (args.size() != 2) return RespParser::encodeError("wrong number of arguments for 'ZCARD'");
    try { return RespParser::encodeInteger(static_cast<long long>(db_.zcard(args[1]))); }
    catch (const std::exception& exception) { return typeError(exception); }
}

std::string CommandHandler::handleDel(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return RespParser::encodeError("wrong number of arguments for 'DEL'");
    }
    std::vector<std::string> keys(args.begin() + 1, args.end());
    size_t deleted = db_.del(keys);
    return RespParser::encodeInteger(static_cast<long long>(deleted));
}

std::string CommandHandler::handleExists(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return RespParser::encodeError("wrong number of arguments for 'EXISTS'");
    }
    std::vector<std::string> keys(args.begin() + 1, args.end());
    size_t count = db_.exists(keys);
    return RespParser::encodeInteger(static_cast<long long>(count));
}

std::string CommandHandler::handleKeys(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return RespParser::encodeError("wrong number of arguments for 'KEYS'");
    }
    std::string pattern = args[1];
    auto key_list = db_.getAllKeys(pattern);

    return RespParser::encodeArray(key_list);
}

std::string CommandHandler::handleSave(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        return RespParser::encodeError("SAVE command takes no arguments");
    }
    if (db_.saveRdb()) {
        return RespParser::encodeSimpleString("OK");
    } else {
        return RespParser::encodeError("ERR Failed to save RDB");
    }
}

std::string CommandHandler::handleAsking(const std::vector<std::string>& args,
                                         cluster::ClientSession& session,
                                         std::uint64_t request_seq) {
    if (args.size() != 1) {
        return RespParser::encodeError("wrong number of arguments for 'ASKING'");
    }
    if (router_ == nullptr) {
        return RespParser::encodeError("This instance has cluster support disabled");
    }
    // 只对紧随其后的那一个命令放行，且仅当目标节点确实处于 importing 状态。
    session.markAskingForNextRequest(request_seq);
    return RespParser::encodeSimpleString("OK");
}

std::string CommandHandler::handleCluster(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return RespParser::encodeError("wrong number of arguments for 'CLUSTER'");
    }
    const std::string subcommand = toUpper(args[1]);

    // KEYSLOT 是纯函数，单节点模式下也允许调用，便于校验分片规则。
    if (subcommand == "KEYSLOT") {
        if (args.size() != 3) {
            return RespParser::encodeError("wrong number of arguments for 'CLUSTER KEYSLOT'");
        }
        return RespParser::encodeInteger(cluster::keyToSlot(args[2]));
    }

    if (router_ == nullptr) {
        return RespParser::encodeError("This instance has cluster support disabled");
    }

    if (subcommand == "SLOTS") {
        if (args.size() != 2) {
            return RespParser::encodeError("wrong number of arguments for 'CLUSTER SLOTS'");
        }
        return handleClusterSlots();
    }
    if (subcommand == "INFO") {
        if (args.size() != 2) {
            return RespParser::encodeError("wrong number of arguments for 'CLUSTER INFO'");
        }
        return handleClusterInfo();
    }
    if (subcommand == "MYID") {
        if (args.size() != 2) {
            return RespParser::encodeError("wrong number of arguments for 'CLUSTER MYID'");
        }
        return RespParser::encodeBulkString(router_->selfId());
    }
    return RespParser::encodeError("Unknown CLUSTER subcommand '" + args[1] + "'");
}

std::string CommandHandler::handleClusterSlots() {
    const cluster::ClusterSlotsView view = router_->clusterSlotsView(db_.nowMs());
    if (!view.complete) {
        // 宁可让客户端重试，也不返回一张会被缓存下来的错误路由表。
        return RespParser::encodeTryAgainError(view.unavailable_reason);
    }

    std::vector<std::string> encoded_ranges;
    encoded_ranges.reserve(view.ranges.size());
    for (const cluster::SlotRangeView& entry : view.ranges) {
        std::vector<std::string> tuple;
        tuple.reserve(3 + entry.replicas.size());
        tuple.push_back(RespParser::encodeInteger(entry.range.start));
        tuple.push_back(RespParser::encodeInteger(entry.range.end));
        // primary 必须是该 Raft group 当前已知的 leader，其余 voter 随后列出。
        tuple.push_back(encodeClusterNode(entry.primary));
        for (const cluster::NodeRecord& replica : entry.replicas) {
            tuple.push_back(encodeClusterNode(replica));
        }
        encoded_ranges.push_back(RespParser::encodeArrayOfEncoded(tuple));
    }
    return RespParser::encodeArrayOfEncoded(encoded_ranges);
}

std::string CommandHandler::handleClusterInfo() {
    const cluster::ClusterStatusView status = router_->statusView(db_.nowMs());

    std::string local_shards;
    for (const cluster::ShardId shard : status.local_shards) {
        if (!local_shards.empty()) {
            local_shards += ',';
        }
        local_shards += std::to_string(shard);
    }

    std::string info;
    info += "cluster_enabled:1\r\n";
    info += std::string("cluster_state:") + (status.state_ok ? "ok" : "fail") + "\r\n";
    info += "cluster_slots_assigned:" + std::to_string(status.slots_assigned) + "\r\n";
    info += "cluster_known_nodes:" + std::to_string(status.known_nodes) + "\r\n";
    info += "cluster_size:" + std::to_string(status.shard_count) + "\r\n";
    info += "cluster_current_epoch:" + std::to_string(status.config_epoch) + "\r\n";
    info += "cluster_my_id:" + router_->selfId() + "\r\n";
    // slot 只是路由单元：Raft group 数量等于分片数量，而不是 16384。
    info += "cluster_slot_space:" + std::to_string(cluster::kSlotCount) + "\r\n";
    info += "cluster_raft_groups:" + std::to_string(status.shard_count) + "\r\n";
    info += "cluster_migrating_slots:" + std::to_string(status.migrating_slots) + "\r\n";
    info += "cluster_my_shards:" + local_shards + "\r\n";
    info += "cluster_my_leader_shards:" + std::to_string(status.local_leader_shards) +
            "\r\n";
    return RespParser::encodeBulkString(info);
}
