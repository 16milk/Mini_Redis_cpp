#include "mini_redis/net/Protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expect_complete(RespParser& parser, const std::string& request,
                     const std::vector<std::string>& expected_arguments) {
    std::vector<std::string> arguments;
    size_t consumed = 0;
    expect(parser.parse(request, arguments, consumed) == RespParser::COMPLETE,
           "request parses completely");
    expect(arguments == expected_arguments, "parsed arguments match");
    expect(consumed == request.size(), "complete request reports exact consumed bytes");
}

} // namespace

int main() {
    RespParser parser;

    const std::string ping = "*1\r\n$4\r\nPING\r\n";
    const std::string set = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    expect_complete(parser, ping, {"PING"});
    expect_complete(parser, set, {"SET", "key", "value"});

    std::vector<std::string> arguments;
    size_t consumed = 0;
    const std::string pipeline = ping + set;
    expect(parser.parse(pipeline, arguments, consumed) == RespParser::COMPLETE,
           "first pipelined request parses");
    expect(arguments == std::vector<std::string>({"PING"}),
           "pipeline returns first command only");
    expect(consumed == ping.size(), "pipeline reports first frame length");

    consumed = 99;
    expect(parser.parse(set.substr(0, set.size() - 2), arguments, consumed) == RespParser::INCOMPLETE,
           "partial bulk string is incomplete");
    expect(consumed == 0, "incomplete request consumes no bytes");

    expect(parser.parse("*x\r\n", arguments, consumed) == RespParser::ERROR,
           "non-numeric array count is rejected");
    expect(parser.parse("*1\r\n$-2\r\n", arguments, consumed) == RespParser::ERROR,
           "invalid negative bulk length is rejected");
    expect(parser.parse("*1\r\n$4\r\nPINGxx", arguments, consumed) == RespParser::ERROR,
           "invalid bulk string terminator is rejected");
    expect(parser.parse("*999999999999999999999999\r\n", arguments, consumed) == RespParser::ERROR,
           "overflowing array count is rejected");

    std::cout << "RESP parser tests passed" << std::endl;
    return EXIT_SUCCESS;
}
