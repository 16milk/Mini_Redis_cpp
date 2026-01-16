// Command.hpp
#pragma once
#include <vector>
#include <string>
#include "Protocol.hpp"  // 用于编码响应

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

    std::string handleHSet(const std::vector<std::string>& args);
    std::string handleHGet(const std::vector<std::string>& args);

    std::string handleDel(const std::vector<std::string>& args);
    std::string handleExists(const std::vector<std::string>& args);
    std::string handleKeys(const std::vector<std::string>& args);

    std::string handleSave(const std::vector<std::string>& args);

};