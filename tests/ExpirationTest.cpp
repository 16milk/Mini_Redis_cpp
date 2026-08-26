#include "mini_redis/core/Database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void fail(const std::string& description) {
    std::cerr << "FAILED: " << description << std::endl;
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const std::string& description) {
    if (!condition) {
        fail(description);
    }
}

template <typename Exception, typename Function>
void expectThrows(Function&& function, const std::string& description) {
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (const std::exception& exception) {
        fail(description + " (unexpected exception: " + exception.what() + ")");
    }
    fail(description + " (no exception)");
}

void expectIndexes(const Database& database, std::size_t expected,
                   const std::string& description) {
    expect(database.expireKeyCount() == expected, description + " expire map size");
    expect(database.scheduledExpireCount() == expected,
           description + " schedule size");
    expect(database.validateExpirationIndexes(), description + " index invariants");
}

void testLazyExpirationBoundaryAndRounding() {
    UnixMillis now = 1'700'000'000'000;
    Database database(true, [&now] { return now; });
    std::string value;

    database.set("boundary", "value");
    expect(database.expire("boundary", 2'000), "install expiration");
    expectIndexes(database, 1, "installed expiration");
    expect(database.ttl("boundary", true) == 2'000, "initial PTTL is exact");
    expect(database.ttl("boundary", false) == 2, "initial TTL is two seconds");

    now += 500;
    expect(database.ttl("boundary", true) == 1'500, "PTTL follows fake clock");
    expect(database.ttl("boundary", false) == 2,
           "TTL rounds 1500 milliseconds to two seconds");
    ++now;
    expect(database.ttl("boundary", false) == 1,
           "TTL rounds 1499 milliseconds to one second");

    now = 1'700'000'001'999;
    expect(database.get("boundary", value) && value == "value",
           "key is visible one millisecond before deadline");
    expect(database.ttl("boundary", true) == 1,
           "one millisecond remains before deadline");

    ++now;
    expect(!database.get("boundary", value), "key is absent exactly at deadline");
    expect(database.ttl("boundary", true) == -2, "expired key has missing PTTL");
    expect(database.physicalKeyCount() == 0, "lazy expiration removes physical object");
    expectIndexes(database, 0, "lazy expiration cleanup");
    expect(database.expirationStats().expired_keys_total == 1,
           "lazy expiration increments total statistic");
    expect(database.expirationStats().lazy_expired_keys_total == 1,
           "lazy expiration increments lazy statistic");
}

void testEveryObjectTypeLazilyExpires() {
    UnixMillis now = 5'000;
    Database database(true, [&now] { return now; });

    database.set("string", "value");
    database.hset("hash", {{"field", "value"}});
    database.rpush("list", {"value"});
    database.sadd("set", {"value"});
    database.zadd("zset", {{1.0, "value"}});
    for (const char* key : {"string", "hash", "list", "set", "zset"}) {
        expect(database.expire(key, 100), std::string("expire " ) + key);
    }
    expectIndexes(database, 5, "five object expirations");

    now += 100;
    std::string value;
    double score = 0;
    expect(!database.get("string", value), "expired string read is missing");
    expect(!database.hget("hash", "field", value), "expired hash read is missing");
    expect(!database.lindex("list", 0, value), "expired list read is missing");
    expect(!database.sismember("set", "value"), "expired set read is missing");
    expect(!database.zscore("zset", "value", score), "expired zset read is missing");
    expect(database.physicalKeyCount() == 0, "all expired object types are removed");
    expectIndexes(database, 0, "all object types lazy cleanup");
}

void testExpiredWrongTypesAndMutationRecreateKeys() {
    UnixMillis now = 10'000;
    Database database(true, [&now] { return now; });

    database.hset("old-hash", {{"field", "value"}});
    database.set("new-hash", "old");
    database.set("new-lpush", "old");
    database.set("new-rpush", "old");
    database.set("new-set", "old");
    database.set("new-zset", "old");
    for (const char* key : {"old-hash", "new-hash", "new-lpush", "new-rpush",
                            "new-set", "new-zset"}) {
        expect(database.expire(key, 1), std::string("expire " ) + key);
    }

    ++now;
    std::string value;
    expect(!database.get("old-hash", value),
           "expired wrong-type object is missing instead of WRONGTYPE");
    expect(database.hset("new-hash", {{"field", "value"}}) == 1,
           "HSET recreates an expired key");
    expect(database.lpush("new-lpush", {"value"}) == 1,
           "LPUSH recreates an expired key");
    expect(database.rpush("new-rpush", {"value"}) == 1,
           "RPUSH recreates an expired key");
    expect(database.sadd("new-set", {"value"}) == 1,
           "SADD recreates an expired key");
    expect(database.zadd("new-zset", {{1.0, "value"}}) == 1,
           "ZADD recreates an expired key");
    expectIndexes(database, 0, "recreated keys do not inherit expiration");
}

void testLiveCompositeMutationsPreserveExpiration() {
    UnixMillis now = 20'000;
    Database database(true, [&now] { return now; });

    database.hset("hash", {{"one", "1"}});
    database.rpush("list", {"one"});
    database.sadd("set", {"one"});
    database.zadd("zset", {{1.0, "one"}});
    for (const char* key : {"hash", "list", "set", "zset"}) {
        expect(database.expire(key, 1'000), std::string("expire " ) + key);
    }

    now += 250;
    expect(database.hset("hash", {{"two", "2"}}) == 1,
           "HSET mutates live expiring hash");
    expect(database.lpush("list", {"zero"}) == 2,
           "LPUSH mutates live expiring list");
    expect(database.rpush("list", {"two"}) == 3,
           "RPUSH mutates live expiring list");
    expect(database.sadd("set", {"two"}) == 1,
           "SADD mutates live expiring set");
    expect(database.zadd("zset", {{2.0, "two"}}) == 1,
           "ZADD mutates live expiring zset");
    for (const char* key : {"hash", "list", "set", "zset"}) {
        expect(database.ttl(key, true) == 750,
               std::string("mutation preserves PTTL for " ) + key);
    }
    expectIndexes(database, 4, "live composite mutations");
}

void testOverwriteDeleteAndContainerCleanup() {
    UnixMillis now = 30'000;
    Database database(true, [&now] { return now; });
    std::string value;

    database.set("overwrite", "old");
    expect(database.expire("overwrite", 100), "expire value before overwrite");
    database.set("overwrite", "new");
    expect(database.ttl("overwrite", true) == -1, "plain SET clears old TTL");
    now += 100;
    expect(database.get("overwrite", value) && value == "new",
           "plain SET value survives old deadline");

    database.set("expired-overwrite", "old");
    expect(database.expire("expired-overwrite", 1),
           "expire value before lazy overwrite");
    ++now;
    database.set("expired-overwrite", "new");
    expect(database.get("expired-overwrite", value) && value == "new",
           "SET lazily expires an old value before overwriting it");
    expect(database.expirationStats().lazy_expired_keys_total == 1,
           "SET overwrite records lazy expiration");

    database.set("ttl-set", "value", 500);
    expect(database.ttl("ttl-set", true) == 500, "SET with TTL installs expiration");
    expect(database.del({"ttl-set"}) == 1, "DEL removes expiring key");
    expectIndexes(database, 0, "DEL removes expiration metadata");

    database.rpush("list", {"one"});
    expect(database.expire("list", 1'000), "expire list");
    expect(database.lpop("list", value) && value == "one", "empty list via LPOP");
    expectIndexes(database, 0, "empty list removes expiration metadata");

    database.sadd("set", {"one"});
    expect(database.expire("set", 1'000), "expire set");
    expect(database.srem("set", {"one"}) == 1, "empty set via SREM");
    expectIndexes(database, 0, "empty set removes expiration metadata");

    database.zadd("zset", {{1.0, "one"}});
    expect(database.expire("zset", 1'000), "expire zset");
    expect(database.zrem("zset", {"one"}) == 1, "empty zset via ZREM");
    expectIndexes(database, 0, "empty zset removes expiration metadata");

    database.set("reused", "old");
    expect(database.expire("reused", 100), "expire key before deletion");
    expect(database.del({"reused"}) == 1, "delete old key");
    database.set("reused", "new");
    now += 100;
    const auto cycle = database.activeExpireCycle(64, std::chrono::seconds(1));
    expect(cycle.deleted == 0, "old deadline cannot delete same-name replacement");
    expect(database.get("reused", value) && value == "new",
           "same-name replacement remains visible");
}

void testReschedulePersistAndBoundedIndexes() {
    UnixMillis now = 40'000;
    Database database(true, [&now] { return now; });
    database.set("key", "value");

    expect(database.expire("key", 100), "set initial TTL");
    expect(database.nextExpireAt() == now + 100, "initial next deadline");
    expect(database.expire("key", 200), "move TTL later");
    expect(database.nextExpireAt() == now + 200, "later deadline replaces old deadline");
    expectIndexes(database, 1, "move deadline later");
    expect(database.expire("key", 200), "repeat identical TTL");
    expectIndexes(database, 1, "identical TTL does not duplicate schedule");
    expect(database.expire("key", 50), "move TTL earlier");
    expect(database.nextExpireAt() == now + 50, "earlier deadline replaces old deadline");
    expectIndexes(database, 1, "move deadline earlier");

    for (int ttl = 1; ttl <= 500; ++ttl) {
        expect(database.expire("key", ttl), "repeatedly renew hot key");
    }
    expectIndexes(database, 1, "hot-key renewals remain memory bounded");
    expect(database.ttl("key", true) == 500, "last renewal wins");
    expect(database.persist("key"), "PERSIST removes an existing TTL");
    expect(!database.persist("key"), "PERSIST reports no TTL on permanent key");
    expect(database.ttl("key", true) == -1, "persisted key is permanent");
    expect(!database.nextExpireAt().has_value(), "no next deadline after PERSIST");
    expectIndexes(database, 0, "PERSIST removes both expiration indexes");
}

void testExistsKeysAndImmediateExpiration() {
    UnixMillis now = 50'000;
    Database database(true, [&now] { return now; });

    database.set("alive", "value");
    database.set("due", "value");
    database.set("future", "value");
    expect(database.expire("due", 100), "expire due key");
    expect(database.expire("future", 200), "expire future key");
    now += 100;

    expect(database.exists({"alive", "alive", "due", "future"}) == 3,
           "EXISTS counts duplicates and excludes expired keys");
    auto keys = database.getAllKeys();
    std::sort(keys.begin(), keys.end());
    expect(keys == std::vector<std::string>({"alive", "future"}),
           "KEYS excludes expired keys");
    expectIndexes(database, 1, "full key scans clean expired metadata");

    expect(database.expire("future", 0), "zero TTL immediately deletes existing key");
    expect(!database.keyExists("future"), "zero TTL key is absent");
    expect(!database.expire("missing", 0), "zero TTL reports false for missing key");
    expect(database.ttl("missing", true) == -2, "missing key TTL sentinel");
    expectIndexes(database, 0, "immediate expiration cleanup");
}

void testActiveExpirationBudgets() {
    UnixMillis now = 60'000;
    Database database(true, [&now] { return now; });

    auto result = database.activeExpireCycle(2, std::chrono::seconds(1));
    expect(result.examined == 0 && result.deleted == 0 && !result.more_due &&
               !result.next_deadline.has_value(),
           "active cycle handles empty schedule");

    for (const char* key : {"c", "a", "b"}) {
        database.set(key, "value");
        expect(database.expire(key, 100), std::string("expire active-cycle key " ) + key);
    }
    now += 100;

    result = database.activeExpireCycle(10, std::chrono::microseconds::zero());
    expect(result.examined == 0 && result.deleted == 0 && result.more_due,
           "zero runtime budget leaves due work queued");
    expect(result.next_deadline == now, "zero runtime budget reports due deadline");
    expectIndexes(database, 3, "zero runtime budget preserves indexes");

    result = database.activeExpireCycle(2, std::chrono::seconds(1));
    expect(result.examined == 2 && result.deleted == 2 && result.more_due,
           "key budget bounds an active expiration cycle");
    expect(result.next_deadline == now, "bounded cycle reports remaining due work");
    expect(database.physicalKeyCount() == 1, "bounded cycle leaves one physical key");
    expectIndexes(database, 1, "bounded active cycle");

    result = database.activeExpireCycle(2, std::chrono::seconds(1));
    expect(result.examined == 1 && result.deleted == 1 && !result.more_due &&
               !result.next_deadline.has_value(),
           "next active cycle drains due backlog");
    expect(database.physicalKeyCount() == 0, "active expiration removes all due objects");
    expectIndexes(database, 0, "drained active cycle");
    expect(database.expirationStats().active_expired_keys_total == 3,
           "active expiration statistic counts deletions");
    expect(database.expirationStats().active_expire_budget_exhausted_total >= 2,
           "active expiration records runtime and key budget exhaustion");
    expect(database.expirationStats().expired_keys_total == 3,
           "active expiration contributes to total expired-key statistics");
}

void testOverflowAndMemoryAccounting() {
    UnixMillis now = std::numeric_limits<UnixMillis>::max() - 5;
    Database database(true, [&now] { return now; });
    database.set("key", "value");

    expectThrows<std::overflow_error>([&database] { database.expire("key", 10); },
                                      "EXPIRE rejects deadline overflow");
    expect(database.keyExists("key"), "overflow leaves original key unchanged");
    expect(database.ttl("key", true) == -1, "overflow leaves original key permanent");
    expectThrows<std::overflow_error>([&database] { database.set("other", "value", 10); },
                                      "SET with TTL rejects deadline overflow");
    expect(!database.keyExists("other"), "failed SET with TTL does not create key");
    expectIndexes(database, 0, "overflow preserves indexes");

    now = 70'000;
    const std::size_t without_expiration = database.memory_usage();
    expect(database.expire("key", 1'000), "install TTL for memory accounting");
    expect(database.memory_usage() > without_expiration,
           "memory usage includes expiration metadata");

    UnixMillis rewound_now = 0;
    Database rewound(true, [&rewound_now] { return rewound_now; });
    rewound.set("far-future", "value");
    expect(rewound.expire("far-future", std::numeric_limits<UnixMillis>::max()),
           "install maximum representable deadline");
    rewound_now = std::numeric_limits<UnixMillis>::min();
    expect(rewound.ttl("far-future", true) ==
               std::numeric_limits<long long>::max(),
           "PTTL saturates after an extreme wall-clock rewind");
    expect(rewound.ttl("far-future", false) ==
               std::numeric_limits<long long>::max() / 1000,
           "TTL saturates in seconds after an extreme wall-clock rewind");
}

} // namespace

int main() {
    testLazyExpirationBoundaryAndRounding();
    testEveryObjectTypeLazilyExpires();
    testExpiredWrongTypesAndMutationRecreateKeys();
    testLiveCompositeMutationsPreserveExpiration();
    testOverwriteDeleteAndContainerCleanup();
    testReschedulePersistAndBoundedIndexes();
    testExistsKeysAndImmediateExpiration();
    testActiveExpirationBudgets();
    testOverflowAndMemoryAccounting();

    std::cout << "expiration tests passed" << std::endl;
    return EXIT_SUCCESS;
}
