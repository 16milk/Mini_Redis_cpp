#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect_equal(const std::string& actual, const std::string& expected,
                  const std::string& description) {
    if (actual != expected) {
        std::cerr << "FAILED: " << description << "\nExpected: " << expected
                  << "\nActual:   " << actual << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::string execute(CommandHandler& handler, std::initializer_list<const char*> values) {
    std::vector<std::string> arguments;
    arguments.reserve(values.size());
    for (const char* value : values) {
        arguments.emplace_back(value);
    }
    return handler.execute(arguments);
}

const std::string kWrongType =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

} // namespace

int main() {
    Database database(true);
    CommandHandler commands(database);

    expect_equal(execute(commands, {"PING"}), "+PONG\r\n", "PING without message");
    expect_equal(execute(commands, {"PING", "hello"}), "$5\r\nhello\r\n",
                 "PING echoes message");
    expect_equal(execute(commands, {"PING", "one", "two"}),
                 "-ERR wrong number of arguments for 'PING'\r\n",
                 "PING rejects extra arguments");

    expect_equal(execute(commands, {"SET", "name", "alice", "EX", "10"}),
                 "-ERR wrong number of arguments for 'SET'\r\n",
                 "SET rejects unimplemented options");
    expect_equal(execute(commands, {"SET", "name", "alice"}), "+OK\r\n",
                 "SET stores exact argument count");
    expect_equal(execute(commands, {"GET", "name"}), "$5\r\nalice\r\n",
                 "GET returns stored string");
    expect_equal(execute(commands, {"GET", "missing"}), "$-1\r\n",
                 "GET distinguishes missing key");

    expect_equal(execute(commands, {"HSET", "profile", "name", "alice", "city", "shanghai"}),
                 ":2\r\n", "HSET counts multiple new fields");
    expect_equal(execute(commands, {"HSET", "profile", "name", "bob", "age", "18"}),
                 ":1\r\n", "HSET counts updates separately from new fields");
    expect_equal(execute(commands, {"HSET", "profile", "name", "carol", "name", "dave"}),
                 ":0\r\n", "HSET processes duplicate fields in request order");
    expect_equal(execute(commands, {"HGET", "profile", "name"}), "$4\r\ndave\r\n",
                 "HSET leaves last duplicate field value");
    expect_equal(execute(commands, {"HGET", "profile", "missing"}), "$-1\r\n",
                 "HGET distinguishes missing field");
    expect_equal(execute(commands, {"HSET", "profile", "field"}),
                 "-ERR wrong number of arguments for 'HSET'\r\n",
                 "HSET rejects unpaired field/value");

    expect_equal(execute(commands, {"GET", "profile"}), kWrongType,
                 "GET reports wrong type instead of null");
    expect_equal(execute(commands, {"HGET", "name", "field"}), kWrongType,
                 "HGET reports wrong type instead of null");
    expect_equal(execute(commands, {"HSET", "name", "field", "value"}), kWrongType,
                 "HSET reports wrong type");

    expect_equal(execute(commands, {"LPUSH", "items", "one"}), ":1\r\n",
                 "LPUSH creates list");
    expect_equal(execute(commands, {"SCARD", "items"}), kWrongType,
                 "existing extended command preserves wrong-type behavior");

    std::cout << "command semantics tests passed" << std::endl;
    return EXIT_SUCCESS;
}
