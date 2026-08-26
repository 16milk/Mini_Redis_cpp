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
    UnixMillis now = 1'700'000'000'000;
    Database database(true, [&now] { return now; });
    CommandHandler commands(database);

    expect_equal(execute(commands, {"PING"}), "+PONG\r\n", "PING without message");
    expect_equal(execute(commands, {"PING", "hello"}), "$5\r\nhello\r\n",
                 "PING echoes message");
    expect_equal(execute(commands, {"PING", "one", "two"}),
                 "-ERR wrong number of arguments for 'PING'\r\n",
                 "PING rejects extra arguments");

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

    expect_equal(execute(commands, {"TTL", "missing"}), ":-2\r\n",
                 "TTL reports a missing key");
    expect_equal(execute(commands, {"PTTL", "missing"}), ":-2\r\n",
                 "PTTL reports a missing key");
    expect_equal(execute(commands, {"TTL", "name"}), ":-1\r\n",
                 "TTL reports a permanent key");
    expect_equal(execute(commands, {"PERSIST", "name"}), ":0\r\n",
                 "PERSIST reports a key without TTL");
    expect_equal(execute(commands, {"PERSIST", "missing"}), ":0\r\n",
                 "PERSIST reports a missing key");

    expect_equal(execute(commands, {"EXPIRE", "name", "2"}), ":1\r\n",
                 "EXPIRE installs a second-based TTL");
    expect_equal(execute(commands, {"PTTL", "name"}), ":2000\r\n",
                 "PTTL exposes EXPIRE in milliseconds");
    expect_equal(execute(commands, {"TTL", "name"}), ":2\r\n",
                 "TTL exposes EXPIRE in seconds");
    now += 500;
    expect_equal(execute(commands, {"TTL", "name"}), ":2\r\n",
                 "TTL rounds 1500 milliseconds up to two seconds");
    ++now;
    expect_equal(execute(commands, {"TTL", "name"}), ":1\r\n",
                 "TTL rounds 1499 milliseconds down to one second");
    expect_equal(execute(commands, {"PEXPIRE", "name", "250"}), ":1\r\n",
                 "PEXPIRE replaces an existing TTL");
    expect_equal(execute(commands, {"PTTL", "name"}), ":250\r\n",
                 "PTTL returns the replacement TTL exactly");
    expect_equal(execute(commands, {"PERSIST", "name"}), ":1\r\n",
                 "PERSIST removes an existing TTL");
    expect_equal(execute(commands, {"PTTL", "name"}), ":-1\r\n",
                 "persisted key becomes permanent");

    expect_equal(execute(commands, {"PEXPIRE", "missing", "1"}), ":0\r\n",
                 "PEXPIRE reports a missing key");
    expect_equal(execute(commands, {"SET", "immediate", "value"}), "+OK\r\n",
                 "prepare immediate expiration");
    expect_equal(execute(commands, {"EXPIRE", "immediate", "0"}), ":1\r\n",
                 "EXPIRE zero deletes an existing key");
    expect_equal(execute(commands, {"GET", "immediate"}), "$-1\r\n",
                 "zero-expired key is absent");
    expect_equal(execute(commands, {"SET", "immediate-negative", "value"}),
                 "+OK\r\n", "prepare negative expiration");
    expect_equal(execute(commands, {"PEXPIRE", "immediate-negative", "-1"}),
                 ":1\r\n", "negative PEXPIRE deletes an existing key");

    expect_equal(execute(commands, {"SET", "seconds", "value", "EX", "2"}),
                 "+OK\r\n", "SET EX stores a value with a TTL");
    expect_equal(execute(commands, {"PTTL", "seconds"}), ":2000\r\n",
                 "SET EX converts seconds to milliseconds");
    expect_equal(execute(commands, {"SET", "milliseconds", "value", "px", "250"}),
                 "+OK\r\n", "SET PX option is case insensitive");
    expect_equal(execute(commands, {"PTTL", "milliseconds"}), ":250\r\n",
                 "SET PX preserves millisecond precision");
    expect_equal(execute(commands, {"SET", "milliseconds", "replacement"}),
                 "+OK\r\n", "plain SET overwrites an expiring key");
    expect_equal(execute(commands, {"TTL", "milliseconds"}), ":-1\r\n",
                 "plain SET clears an existing TTL");

    expect_equal(execute(commands, {"SET", "atomic", "old", "PX", "500"}),
                 "+OK\r\n", "prepare value for SET option validation");
    const std::string integer_error = "-ERR value is not an integer or out of range\r\n";
    const std::string set_expire_error =
        "-ERR invalid expire time in 'set' command\r\n";
    const std::string syntax_error = "-ERR syntax error\r\n";
    expect_equal(execute(commands, {"SET", "atomic", "new", "EX", "0"}),
                 set_expire_error, "SET EX rejects zero timeout");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX", "-1"}),
                 set_expire_error, "SET PX rejects negative timeout");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX", "nope"}),
                 integer_error, "SET PX rejects a non-integer timeout");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX", "+1"}),
                 integer_error, "SET PX rejects a leading plus sign");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX", "01"}),
                 integer_error, "SET PX rejects a noncanonical leading zero");
    expect_equal(execute(commands, {"SET", "atomic", "new", "EX",
                                    "9223372036854775807"}),
                 set_expire_error, "SET EX rejects millisecond conversion overflow");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX",
                                    "9223372036854775807"}),
                 set_expire_error, "SET PX rejects absolute deadline overflow");
    expect_equal(execute(commands, {"SET", "atomic", "new", "NX", "1"}),
                 syntax_error, "SET rejects an unsupported option");
    expect_equal(execute(commands, {"SET", "atomic", "new", "EX", "1",
                                    "PX", "1"}),
                 syntax_error, "SET rejects conflicting expiration options");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX", "1",
                                    "PX", "2"}),
                 syntax_error, "SET rejects repeated expiration options");
    expect_equal(execute(commands, {"SET", "atomic", "new", "PX", "1", "extra"}),
                 syntax_error, "SET rejects trailing options");
    expect_equal(execute(commands, {"GET", "atomic"}), "$3\r\nold\r\n",
                 "invalid SET options preserve the old value");
    expect_equal(execute(commands, {"PTTL", "atomic"}), ":500\r\n",
                 "invalid SET options preserve the old TTL");

    expect_equal(execute(commands, {"EXPIRE", "atomic", "nope"}), integer_error,
                 "EXPIRE rejects a non-integer timeout");
    expect_equal(execute(commands, {"EXPIRE", "atomic", "+1"}), integer_error,
                 "EXPIRE rejects a leading plus sign");
    expect_equal(execute(commands, {"PEXPIRE", "atomic",
                                    "9223372036854775808"}),
                 integer_error, "PEXPIRE rejects an out-of-range timeout");
    expect_equal(execute(commands, {"EXPIRE", "atomic",
                                    "9223372036854775807"}),
                 "-ERR invalid expire time in 'expire' command\r\n",
                 "EXPIRE rejects millisecond conversion overflow");
    expect_equal(execute(commands, {"PEXPIRE", "atomic",
                                    "9223372036854775807"}),
                 "-ERR invalid expire time in 'pexpire' command\r\n",
                 "PEXPIRE rejects absolute deadline overflow");
    expect_equal(execute(commands, {"PEXPIRE", "missing",
                                    "9223372036854775807"}),
                 "-ERR invalid expire time in 'pexpire' command\r\n",
                 "PEXPIRE validates deadline overflow before key existence");
    expect_equal(execute(commands, {"GET", "atomic"}), "$3\r\nold\r\n",
                 "invalid EXPIRE leaves the value unchanged");
    expect_equal(execute(commands, {"PTTL", "atomic"}), ":500\r\n",
                 "invalid EXPIRE leaves the TTL unchanged");

    expect_equal(execute(commands, {"SET", "expired-string", "value", "PX", "1"}),
                 "+OK\r\n", "prepare an expired string");
    expect_equal(execute(commands, {"HSET", "expired-hash", "field", "value"}),
                 ":1\r\n", "prepare an expired hash");
    expect_equal(execute(commands, {"PEXPIRE", "expired-hash", "1"}), ":1\r\n",
                 "expire hash used for wrong-type regression");
    expect_equal(execute(commands, {"RPUSH", "expired-list", "value"}), ":1\r\n",
                 "prepare an expired list");
    expect_equal(execute(commands, {"PEXPIRE", "expired-list", "1"}), ":1\r\n",
                 "expire list");
    expect_equal(execute(commands, {"SADD", "expired-set", "value"}), ":1\r\n",
                 "prepare an expired set");
    expect_equal(execute(commands, {"PEXPIRE", "expired-set", "1"}), ":1\r\n",
                 "expire set");
    expect_equal(execute(commands, {"ZADD", "expired-zset", "1", "value"}),
                 ":1\r\n", "prepare an expired zset");
    expect_equal(execute(commands, {"PEXPIRE", "expired-zset", "1"}), ":1\r\n",
                 "expire zset");
    ++now;
    expect_equal(execute(commands, {"GET", "expired-string"}), "$-1\r\n",
                 "GET treats expired string as missing");
    expect_equal(execute(commands, {"GET", "expired-hash"}), "$-1\r\n",
                 "type checking happens after expiration");
    expect_equal(execute(commands, {"HGET", "expired-hash", "field"}), "$-1\r\n",
                 "HGET treats expired hash as missing");
    expect_equal(execute(commands, {"LLEN", "expired-list"}), ":0\r\n",
                 "LLEN treats expired list as missing");
    expect_equal(execute(commands, {"SCARD", "expired-set"}), ":0\r\n",
                 "SCARD treats expired set as missing");
    expect_equal(execute(commands, {"ZCARD", "expired-zset"}), ":0\r\n",
                 "ZCARD treats expired zset as missing");

    expect_equal(execute(commands, {"SET", "expired-multi", "value", "PX", "1"}),
                 "+OK\r\n", "prepare expired key for multi-key commands");
    expect_equal(execute(commands, {"SET", "live-multi", "value"}), "+OK\r\n",
                 "prepare live key for multi-key commands");
    ++now;
    expect_equal(execute(commands, {"EXISTS", "expired-multi", "live-multi",
                                    "live-multi"}),
                 ":2\r\n", "EXISTS excludes expired keys and counts duplicates");
    expect_equal(execute(commands, {"DEL", "expired-multi", "live-multi"}), ":1\r\n",
                 "DEL excludes an expired key from its deletion count");

    expect_equal(execute(commands, {"EXPIRE", "key-only"}),
                 "-ERR wrong number of arguments for 'EXPIRE'\r\n",
                 "EXPIRE validates arity");
    expect_equal(execute(commands, {"PEXPIRE", "key-only"}),
                 "-ERR wrong number of arguments for 'PEXPIRE'\r\n",
                 "PEXPIRE validates arity");
    expect_equal(execute(commands, {"TTL"}),
                 "-ERR wrong number of arguments for 'TTL'\r\n",
                 "TTL validates arity");
    expect_equal(execute(commands, {"PTTL", "one", "two"}),
                 "-ERR wrong number of arguments for 'PTTL'\r\n",
                 "PTTL validates arity");
    expect_equal(execute(commands, {"PERSIST"}),
                 "-ERR wrong number of arguments for 'PERSIST'\r\n",
                 "PERSIST validates arity");

    std::cout << "command semantics tests passed" << std::endl;
    return EXIT_SUCCESS;
}
