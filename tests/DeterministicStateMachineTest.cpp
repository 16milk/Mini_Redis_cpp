#include "mini_redis/cluster/Slot.hpp"
#include "mini_redis/command/CanonicalCommand.hpp"
#include "mini_redis/core/Database.hpp"

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

command::CanonicalizeContext context(std::uint64_t epoch = 7,
                                     UnixMillis logical_time = 1'000'000) {
    command::CanonicalizeContext result;
    result.shard = 3;
    result.slot_epoch = epoch;
    result.logical_time_ms = logical_time;
    return result;
}

command::CanonicalizeResult canonical(
    std::initializer_list<std::string> arguments,
    const command::CanonicalizeContext& apply_context = context()) {
    return command::canonicalizeWrite(
        std::vector<std::string>(arguments), apply_context);
}

void testValidationAndNormalization() {
    const auto cross_slot = canonical({"DEL", "foo", "bar"});
    expect(!cross_slot.ok && cross_slot.error == "CROSSSLOT",
           "cross-slot writes are rejected before Raft");

    expect(!canonical({"ZADD", "scores", "nan", "alice"}).ok,
           "NaN is rejected before Raft");
    expect(!canonical({"ZADD", "scores", "+inf", "alice"}).ok,
           "infinity is rejected before Raft");
    expect(!canonical({"LREM", "items", "999999999999999999999", "x"}).ok,
           "out-of-range integers are rejected before Raft");
    expect(!canonical({"GET", "key"}).ok, "read commands cannot enter the write log");
    expect(!canonical({"SET", "key"}).ok, "arity is checked before Raft");

    const auto negative_zero = canonical({"ZADD", "scores", "-0", "alice"});
    expect(negative_zero.ok, "finite float is accepted");
    expect(negative_zero.command.arguments[1].float_bits == 0,
           "negative zero has one canonical IEEE-754 representation");
}

void testBinaryCodec() {
    const std::string key("bin\0{tag}", 9);
    const std::string value("a\0b\r\nc", 6);
    auto built = canonical({"SET", key, value});
    expect(built.ok, "binary-safe SET canonicalizes");

    const std::string encoded = command::encodeCanonicalCommand(built.command);
    expect(encoded.rfind("*", 0) != 0, "the log does not contain a RESP array");

    command::CanonicalCommand decoded;
    std::string error;
    expect(command::decodeCanonicalCommand(encoded, decoded, error),
           "canonical binary command decodes");
    expect(decoded.arguments[0].bytes == key && decoded.arguments[1].bytes == value,
           "explicit lengths preserve embedded NUL and CRLF");

    std::string trailing = encoded;
    trailing.push_back('x');
    expect(!command::decodeCanonicalCommand(trailing, decoded, error),
           "trailing bytes are rejected deterministically");
}

void testDeterministicTimeAndEpochFence() {
    UnixMillis fast_clock = 9'000'000;
    UnixMillis slow_clock = 1;
    Database first(true, [&] { return fast_clock; });
    Database second(true, [&] { return slow_clock; });
    command::DeterministicStateMachine first_state(first, 3);
    command::DeterministicStateMachine second_state(second, 3);

    const auto built = canonical({"SET", "{user}:key", "value", "PX", "500"});
    expect(built.ok, "expiring SET canonicalizes");
    const std::string payload = command::encodeCanonicalCommand(built.command);
    const command::SlotOwnership current{3, 7};

    expect(first_state.apply(payload, current).status == command::ApplyStatus::kApplied,
           "first replica applies");
    expect(second_state.apply(payload, current).status == command::ApplyStatus::kApplied,
           "second replica applies");

    {
        Database::ScopedLogicalTime first_now(first, 1'000'100);
        Database::ScopedLogicalTime second_now(second, 1'000'100);
        expect(first.ttl("{user}:key", true) == 400,
               "deadline is based on replicated logical time");
        expect(second.ttl("{user}:key", true) == 400,
               "different wall clocks produce the same state-machine result");
    }

    Database stale_database(true);
    command::DeterministicStateMachine stale_state(stale_database, 3);
    const auto stale = stale_state.apply(payload, command::SlotOwnership{3, 8});
    expect(stale.status == command::ApplyStatus::kNoOpStaleEpoch,
           "an old ownership epoch becomes a deterministic no-op");
    {
        Database::ScopedLogicalTime now(stale_database, 1'000'100);
        expect(!stale_database.keyExists("{user}:key"),
               "stale command did not mutate the database");
    }
}

void testAtomicSameSlotMultiKeyWrite() {
    Database database(true, [] { return UnixMillis{0}; });
    command::DeterministicStateMachine state(database, 3);
    database.set("{account}:a", "1");
    database.set("{account}:b", "2");

    const auto deletion = canonical({"DEL", "{account}:a", "{account}:b"});
    expect(deletion.ok, "same-slot DEL is one canonical command");
    const std::string payload = command::encodeCanonicalCommand(deletion.command);

    const auto stale = state.apply(payload, command::SlotOwnership{3, 8});
    expect(stale.status == command::ApplyStatus::kNoOpStaleEpoch,
           "epoch mismatch rejects the whole multi-key command");
    expect(database.keyExists("{account}:a") && database.keyExists("{account}:b"),
           "no subset is changed on rejection");

    const auto applied = state.apply(payload, command::SlotOwnership{3, 7});
    expect(applied.status == command::ApplyStatus::kApplied &&
               applied.has_integer && applied.integer == 2,
           "the full same-slot command applies once");
    expect(!database.keyExists("{account}:a") && !database.keyExists("{account}:b"),
           "all keys were deleted together");
}

} // namespace

int main() {
    testValidationAndNormalization();
    testBinaryCodec();
    testDeterministicTimeAndEpochFence();
    testAtomicSameSlotMultiKeyWrite();
    std::cout << "DeterministicStateMachineTest passed" << std::endl;
    return EXIT_SUCCESS;
}
