// Command.cpp
#include "Command.hpp"
#include "Database.hpp"
#include <algorithm>
#include <cctype>
#include <string>

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

std::string CommandHandler::handlePing(const std::vector<std::string>& /*args*/) {
    return RespParser::encodeSimpleString("PONG");
}

std::string CommandHandler::handleSet(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        return RespParser::encodeError("wrong number of arguments for 'SET'");
    }
    // 支持 SET key value [EX seconds] ... 但阶段三只取前两个
    db_.set(args[1], args[2]);
    return RespParser::encodeSimpleString("OK");
}

std::string CommandHandler::handleGet(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return RespParser::encodeError("wrong number of arguments for 'GET'");
    }
    std::string value;
    if (db_.get(args[1], value)) {
        return RespParser::encodeBulkString(value);
    } else {
        return RespParser::encodeNullBulkString(); // $-1\r\n
    }
}

// 在 Command.cpp 中添加
std::string CommandHandler::handleHSet(const std::vector<std::string>& args) {
    if (args.size() < 4 || (args.size() - 2) % 2 != 0) {
        return RespParser::encodeError("wrong number of arguments for 'HSET'");
    }
    try {
        // 阶段四只处理第一个 field-value 对（简化）
        db_.hset(args[1], args[2], args[3]);
        return RespParser::encodeInteger(1); // Redis 返回新增 field 数
    } catch (const std::exception& e) {
        return RespParser::encodeError(e.what());
    }
}

std::string CommandHandler::handleHGet(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        return RespParser::encodeError("wrong number of arguments for 'HGET'");
    }
    std::string value;
    if (db_.hget(args[1], args[2], value)) {
        return RespParser::encodeBulkString(value);
    } else {
        return RespParser::encodeNullBulkString();
    }
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

    // RESP 数组格式：*N\r\n$M\r\nkey1\r\n$M\r\nkey2\r\n...
    std::string resp = "*" + std::to_string(key_list.size()) + "\r\n";
    for (const auto& key : key_list) {
        resp += RespParser::encodeBulkString(key);
    }
    return resp;
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