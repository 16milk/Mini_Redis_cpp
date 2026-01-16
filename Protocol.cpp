// Protocol.cpp
#include "Protocol.hpp"
#include <cctype>
#include <cstdlib>
#include <iostream>

// 辅助：跳过空白（RESP 不允许空格，但安全起见）
static size_t skipWhitespace(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    return pos;
}

RespParser::ParseResult RespParser::parse(const std::string& input, std::vector<std::string>& args) {
    args.clear();
    size_t pos = 0;
    pos = skipWhitespace(input, pos);
    if (pos >= input.size()) return ParseResult::INCOMPLETE;

    if (input[pos] != '*') {
        return ParseResult::ERROR; // 只支持数组格式（redis-cli 默认发数组）
    }

    // 解析 "*<number>\r\n"
    size_t end = input.find("\r\n", pos);
    if (end == std::string::npos) return ParseResult::INCOMPLETE;

    int argc = std::atoi(input.c_str() + pos + 1);
    if (argc < 0) return ParseResult::ERROR;
    if (argc == 0) {
        // 空命令
        return ParseResult::COMPLETE;
    }

    pos = end + 2; // 跳过 "\r\n"

    for (int i = 0; i < argc; ++i) {
        if (pos >= input.size()) return ParseResult::INCOMPLETE;
        if (input[pos] != '$') return ParseResult::ERROR;

        end = input.find("\r\n", pos);
        if (end == std::string::npos) return ParseResult::INCOMPLETE;

        int len = std::atoi(input.c_str() + pos + 1);
        if (len < -1) return ParseResult::ERROR;

        pos = end + 2; // 跳过 "\r\n"

        if (len == -1) {
            args.emplace_back(); // null string
        } else {
            if (pos + len > input.size()) return ParseResult::INCOMPLETE;
            args.emplace_back(input.substr(pos, len));
            pos += len;
            // 检查是否有 \r\n
            if (pos + 2 > input.size()) return ParseResult::INCOMPLETE;
            if (input[pos] != '\r' || input[pos+1] != '\n') return ParseResult::ERROR;
            pos += 2;
        }
    }

    return ParseResult::COMPLETE;
}

// ========== 编码函数 ==========
std::string RespParser::encodeSimpleString(const std::string& s) {
    return "+" + s + "\r\n";
}

std::string RespParser::encodeBulkString(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string RespParser::encodeError(const std::string& msg) {
    return "-ERR " + msg + "\r\n";
}

std::string RespParser::encodeInteger(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string RespParser::encodeNullBulkString() {
    return "$-1\r\n";
}