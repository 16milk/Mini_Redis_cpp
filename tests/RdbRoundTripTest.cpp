#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include "mini_redis/persistence/Rdb.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void expect(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expect_response(CommandHandler& commands, const std::vector<std::string>& arguments,
                     const std::string& expected, const std::string& description) {
    const std::string actual = commands.execute(arguments);
    expect(actual == expected, description);
}

} // namespace

int main() {
    Database database(true);
    CommandHandler commands(database);

    expect_response(commands, {"SET", "string", "value"}, "+OK\r\n", "store string");
    expect_response(commands, {"HSET", "hash", "field", "value", "other", "two"},
                    ":2\r\n", "store hash");
    expect_response(commands, {"RPUSH", "list", "one", "two"}, ":2\r\n", "store list");
    expect_response(commands, {"SADD", "set", "1", "member"}, ":2\r\n", "store set");
    expect_response(commands, {"ZADD", "zset", "2", "beta", "1", "alpha"},
                    ":2\r\n", "store zset");

    const std::string filename = "mini_redis_rdb_round_trip_" + std::to_string(getpid()) + ".rdb";
    expect(database.saveRdb(filename), "save RDB");
    const auto loaded = RdbEncoder::loadFromFile(filename);
    std::remove(filename.c_str());

    expect(loaded.size() == 5, "all five object types load");
    expect(loaded.at("string")->type() == ObjectType::STRING, "loaded string type");
    expect(loaded.at("hash")->type() == ObjectType::HASH, "loaded hash type");
    expect(loaded.at("list")->type() == ObjectType::LIST, "loaded list type");
    expect(loaded.at("set")->type() == ObjectType::SET, "loaded set type");
    expect(loaded.at("zset")->type() == ObjectType::ZSET, "loaded zset type");

    const auto* string = static_cast<const StringObject*>(loaded.at("string").get());
    const auto* hash = static_cast<const HashObject*>(loaded.at("hash").get());
    const auto* list = static_cast<const ListObject*>(loaded.at("list").get());
    const auto* set = static_cast<const SetObject*>(loaded.at("set").get());
    const auto* zset = static_cast<const ZSetObject*>(loaded.at("zset").get());
    std::string hash_value;

    expect(string->value() == "value", "string value round trips");
    expect(hash->get_field("other", hash_value) && hash_value == "two",
           "hash values round trip");
    expect(list->values() == std::vector<std::string>({"one", "two"}),
           "list values round trip");
    expect(set->contains("1") && set->contains("member"), "set members round trip");
    expect(zset->range(0, -1, true) ==
               std::vector<std::string>({"alpha", "1", "beta", "2"}),
           "zset ordering and scores round trip");

    std::cout << "RDB round-trip tests passed" << std::endl;
    return EXIT_SUCCESS;
}
