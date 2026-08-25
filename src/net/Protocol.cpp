#include "mini_redis/net/Protocol.hpp"
#include <limits>

namespace {

bool parseInteger(const std::string& input, size_t begin, size_t end, long long& output) {
    if (begin == end) {
        return false;
    }

    bool negative = false;
    if (input[begin] == '-' || input[begin] == '+') {
        negative = input[begin] == '-';
        ++begin;
        if (begin == end) {
            return false;
        }
    }

    unsigned long long value = 0;
    const unsigned long long positive_limit =
        static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    const unsigned long long negative_limit = positive_limit + 1ULL;
    const unsigned long long limit = negative ? negative_limit : positive_limit;

    for (size_t index = begin; index < end; ++index) {
        const char character = input[index];
        if (character < '0' || character > '9') {
            return false;
        }

        const unsigned long long digit = static_cast<unsigned long long>(character - '0');
        if (value > (limit - digit) / 10ULL) {
            return false;
        }
        value = value * 10ULL + digit;
    }

    if (negative) {
        if (value == negative_limit) {
            output = std::numeric_limits<long long>::min();
        } else {
            output = -static_cast<long long>(value);
        }
    } else {
        output = static_cast<long long>(value);
    }
    return true;
}

} // namespace

RespParser::ParseResult RespParser::parse(const std::string& input,
                                          std::vector<std::string>& args,
                                          size_t& bytes_consumed) const {
    args.clear();
    bytes_consumed = 0;
    size_t pos = 0;
    if (pos >= input.size()) {
        return ParseResult::INCOMPLETE;
    }

    if (input[pos] != '*') {
        return ParseResult::ERROR; // 仅支持 redis-cli 默认使用的 RESP 数组格式。
    }

    size_t end = input.find("\r\n", pos);
    if (end == std::string::npos) {
        return ParseResult::INCOMPLETE;
    }

    long long argc = 0;
    if (!parseInteger(input, pos + 1, end, argc) || argc < 0 ||
        argc > static_cast<long long>(std::numeric_limits<int>::max())) {
        return ParseResult::ERROR;
    }

    pos = end + 2;
    if (argc == 0) {
        bytes_consumed = pos;
        return ParseResult::COMPLETE;
    }

    for (long long index = 0; index < argc; ++index) {
        if (pos >= input.size()) {
            return ParseResult::INCOMPLETE;
        }
        if (input[pos] != '$') {
            return ParseResult::ERROR;
        }

        end = input.find("\r\n", pos);
        if (end == std::string::npos) {
            return ParseResult::INCOMPLETE;
        }

        long long length = 0;
        if (!parseInteger(input, pos + 1, end, length) || length < -1) {
            return ParseResult::ERROR;
        }

        pos = end + 2;

        if (length == -1) {
            args.emplace_back();
            continue;
        }

        const auto bulk_length = static_cast<size_t>(length);
        if (bulk_length > input.size() - pos) {
            return ParseResult::INCOMPLETE;
        }
        if (input.size() - pos - bulk_length < 2) {
            return ParseResult::INCOMPLETE;
        }

        args.emplace_back(input.substr(pos, bulk_length));
        pos += bulk_length;
        if (input[pos] != '\r' || input[pos + 1] != '\n') {
            return ParseResult::ERROR;
        }
        pos += 2;
    }

    bytes_consumed = pos;
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

std::string RespParser::encodeWrongTypeError() {
    return "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
}

std::string RespParser::encodeInteger(long long n) {
    return ":" + std::to_string(n) + "\r\n";
}

std::string RespParser::encodeNullBulkString() {
    return "$-1\r\n";
}

std::string RespParser::encodeArray(const std::vector<std::string>& values) {
    std::string response = "*" + std::to_string(values.size()) + "\r\n";
    for (const auto& value : values) {
        response += encodeBulkString(value);
    }
    return response;
}
