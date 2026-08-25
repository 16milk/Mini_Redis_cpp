// Command.cpp
#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
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

} // namespace

static std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string CommandHandler::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        return RespParser::encodeError("empty command");
    }

    std::string cmd = toUpper(args[0]);

    if (cmd == "PING") {
        return handlePing(args);
    } else if (cmd == "SET") {
        return handleSet(args);
    } else if (cmd == "GET") {
        return handleGet(args);
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
    if (args.size() != 3) {
        return RespParser::encodeError("wrong number of arguments for 'SET'");
    }
    db_.set(args[1], args[2]);
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
