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

    // 解析输入缓冲区中的第一条命令。
    // 仅在 COMPLETE 时设置 bytes_consumed，调用方可据此保留后续管线化请求。
    ParseResult parse(const std::string& input, std::vector<std::string>& args,
                      size_t& bytes_consumed) const;

    // --- 编码函数（用于构建响应） ---
    static std::string encodeSimpleString(const std::string& s);
    static std::string encodeBulkString(const std::string& s);
    static std::string encodeError(const std::string& msg);
    static std::string encodeWrongTypeError();
    static std::string encodeInteger(long long n);
    static std::string encodeNullBulkString(); // "$-1\r\n"
    static std::string encodeArray(const std::vector<std::string>& values);
};
