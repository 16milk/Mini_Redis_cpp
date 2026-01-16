// Protocol.hpp
#pragma once
#include <string>
#include <vector>
#include <optional>


class RespParser {
public:
    enum ParseResult {
        INCOMPLETE,  // 数据不完整，需继续读
        COMPLETE,    // 解析成功，args 已填充
        ERROR        // 协议错误
    };

    // 解析输入缓冲区，返回结果 + 命令参数（如 {"SET", "key", "val"}）
    ParseResult parse(const std::string& input, std::vector<std::string>& args);

    // --- 编码函数（用于构建响应） ---
    static std::string encodeSimpleString(const std::string& s);
    static std::string encodeBulkString(const std::string& s);
    static std::string encodeError(const std::string& msg);
    static std::string encodeInteger(long long n);
    static std::string encodeNullBulkString(); // "$-1\r\n"
};