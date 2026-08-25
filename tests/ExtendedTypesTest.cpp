#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include "mini_redis/persistence/Rdb.hpp"

#include <cstdio>
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

} // namespace

int main() {
    Database database(true);
    CommandHandler commands(database);

    expect_equal(execute(commands, {"RPUSH", "list", "one", "two", "three"}),
                 ":3\r\n", "RPUSH returns list length");
    expect_equal(execute(commands, {"LPUSH", "list", "zero"}),
                 ":4\r\n", "LPUSH returns list length");
    expect_equal(execute(commands, {"LRANGE", "list", "0", "-1"}),
                 "*4\r\n$4\r\nzero\r\n$3\r\none\r\n$3\r\ntwo\r\n$5\r\nthree\r\n",
                 "LRANGE retains order");
    expect_equal(execute(commands, {"LREM", "list", "1", "two"}),
                 ":1\r\n", "LREM removes a member");
    expect_equal(execute(commands, {"RPOP", "list"}),
                 "$5\r\nthree\r\n", "RPOP removes tail member");

    expect_equal(execute(commands, {"SADD", "set", "1", "2", "1", "01"}),
                 ":3\r\n", "SADD accepts distinct byte strings");
    expect_equal(execute(commands, {"SISMEMBER", "set", "01"}),
                 ":1\r\n", "SISMEMBER finds noncanonical numeric string");
    expect_equal(execute(commands, {"SMEMBERS", "set"}),
                 "*3\r\n$2\r\n01\r\n$1\r\n1\r\n$1\r\n2\r\n",
                 "SMEMBERS returns deterministic ordering");

    expect_equal(execute(commands, {"ZADD", "zset", "2", "beta", "1", "alpha", "2", "gamma"}),
                 ":3\r\n", "ZADD creates members");
    expect_equal(execute(commands, {"ZRANGE", "zset", "0", "-1", "WITHSCORES"}),
                 "*6\r\n$5\r\nalpha\r\n$1\r\n1\r\n$4\r\nbeta\r\n$1\r\n2\r\n$5\r\ngamma\r\n$1\r\n2\r\n",
                 "ZRANGE sorts by score then member");
    expect_equal(execute(commands, {"ZRANGEBYSCORE", "zset", "2", "+inf"}),
                 "*2\r\n$4\r\nbeta\r\n$5\r\ngamma\r\n",
                 "ZRANGEBYSCORE filters the score range");
    expect_equal(execute(commands, {"ZRANK", "zset", "gamma"}),
                 ":2\r\n", "ZRANK returns zero-based rank");

    expect_equal(execute(commands, {"SET", "string", "value"}),
                 "+OK\r\n", "SET creates a string");
    expect_equal(execute(commands, {"SADD", "string", "member"}),
                 "-ERR WRONGTYPE Operation against a key holding the wrong kind of value\r\n",
                 "extended commands reject wrong key types");

    const std::string filename = "mini_redis_extended_types_test.rdb";
    if (!database.saveRdb(filename)) {
        std::cerr << "FAILED: could not save RDB" << std::endl;
        return EXIT_FAILURE;
    }
    const auto loaded = RdbEncoder::loadFromFile(filename);
    std::remove(filename.c_str());

    if (loaded.size() != 4 || loaded.at("list")->type() != ObjectType::LIST ||
        loaded.at("set")->type() != ObjectType::SET ||
        loaded.at("zset")->type() != ObjectType::ZSET) {
        std::cerr << "FAILED: RDB type round trip" << std::endl;
        return EXIT_FAILURE;
    }

    const auto* list = static_cast<const ListObject*>(loaded.at("list").get());
    const auto* set = static_cast<const SetObject*>(loaded.at("set").get());
    const auto* zset = static_cast<const ZSetObject*>(loaded.at("zset").get());
    if (list->values() != std::vector<std::string>({"zero", "one"}) ||
        !set->contains("01") || zset->range(0, -1, true) !=
            std::vector<std::string>({"alpha", "1", "beta", "2", "gamma", "2"})) {
        std::cerr << "FAILED: RDB value round trip" << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "extended type command and RDB tests passed" << std::endl;
    return EXIT_SUCCESS;
}
