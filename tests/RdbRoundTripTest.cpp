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
    UnixMillis now = 1'700'000'000'000;
    Database database(true, [&now] { return now; });
    CommandHandler commands(database);

    expect_response(commands, {"SET", "string", "value"}, "+OK\r\n", "store string");
    expect_response(commands, {"HSET", "hash", "field", "value", "other", "two"},
                    ":2\r\n", "store hash");
    expect_response(commands, {"RPUSH", "list", "one", "two"}, ":2\r\n", "store list");
    expect_response(commands, {"SADD", "set", "1", "member"}, ":2\r\n", "store set");
    expect_response(commands, {"ZADD", "zset", "2", "beta", "1", "alpha"},
                    ":2\r\n", "store zset");
    expect_response(commands, {"SET", "permanent", "kept"}, "+OK\r\n",
                    "store permanent key beside expiring keys");
    expect_response(commands, {"SET", "already-expired", "filtered"}, "+OK\r\n",
                    "store key that expires at the snapshot boundary");

    expect(database.expire("string", 100), "expire string");
    expect(database.expire("hash", 200), "expire hash");
    expect(database.expire("list", 300), "expire list");
    expect(database.expire("set", 400), "expire set");
    expect(database.expire("zset", 500), "expire zset");
    expect(database.expire("already-expired", 1), "expire snapshot-filtered key");

    const UnixMillis string_deadline = now + 100;
    const UnixMillis hash_deadline = now + 200;
    const UnixMillis list_deadline = now + 300;
    const UnixMillis set_deadline = now + 400;
    const UnixMillis zset_deadline = now + 500;
    ++now;

    const std::string filename = "mini_redis_rdb_round_trip_" + std::to_string(getpid()) + ".rdb";
    expect(database.saveRdb(filename), "save RDB");
    const RdbLoadResult loaded = RdbEncoder::loadFromFile(filename, now);

    expect(loaded.objects.size() == 6,
           "all five object types and the permanent key load");
    expect(loaded.objects.count("already-expired") == 0,
           "save filters a key expired at the snapshot time");
    expect(loaded.expires.size() == 5, "all five future deadlines load");
    expect(loaded.expires.at("string") == string_deadline,
           "string absolute deadline round trips");
    expect(loaded.expires.at("hash") == hash_deadline,
           "hash absolute deadline round trips");
    expect(loaded.expires.at("list") == list_deadline,
           "list absolute deadline round trips");
    expect(loaded.expires.at("set") == set_deadline,
           "set absolute deadline round trips");
    expect(loaded.expires.at("zset") == zset_deadline,
           "zset absolute deadline round trips");
    expect(loaded.expires.count("permanent") == 0,
           "permanent key has no expiration metadata");

    expect(loaded.objects.at("string")->type() == ObjectType::STRING,
           "loaded string type");
    expect(loaded.objects.at("hash")->type() == ObjectType::HASH, "loaded hash type");
    expect(loaded.objects.at("list")->type() == ObjectType::LIST, "loaded list type");
    expect(loaded.objects.at("set")->type() == ObjectType::SET, "loaded set type");
    expect(loaded.objects.at("zset")->type() == ObjectType::ZSET, "loaded zset type");

    const auto* string =
        static_cast<const StringObject*>(loaded.objects.at("string").get());
    const auto* hash = static_cast<const HashObject*>(loaded.objects.at("hash").get());
    const auto* list = static_cast<const ListObject*>(loaded.objects.at("list").get());
    const auto* set = static_cast<const SetObject*>(loaded.objects.at("set").get());
    const auto* zset = static_cast<const ZSetObject*>(loaded.objects.at("zset").get());
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

    const RdbLoadResult at_first_deadline =
        RdbEncoder::loadFromFile(filename, string_deadline);
    std::remove(filename.c_str());
    expect(at_first_deadline.objects.size() == 5,
           "load filters a key exactly at its absolute deadline");
    expect(at_first_deadline.objects.count("string") == 0,
           "deadline-equal string is filtered on load");
    expect(at_first_deadline.expires.size() == 4 &&
               at_first_deadline.expires.at("hash") == hash_deadline,
           "later absolute deadlines remain unchanged after reload");

    std::cout << "RDB round-trip tests passed" << std::endl;
    return EXIT_SUCCESS;
}
