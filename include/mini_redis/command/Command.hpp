// Command.hpp
#pragma once
#include <vector>
#include <string>
#include "mini_redis/net/Protocol.hpp"

class Database;

class CommandHandler {
public:
    explicit CommandHandler(Database& db) : db_(db) {}

    // 执行命令，返回 RESP 响应字符串
    std::string execute(const std::vector<std::string>& args);

private:
    Database& db_;

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

};
