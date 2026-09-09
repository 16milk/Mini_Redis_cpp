#pragma once

#include "mini_redis/cluster/Topology.hpp"
#include "mini_redis/core/Expiration.hpp"

#include <cstdint>
#include <string>
#include <vector>

class Database;

namespace cluster {
struct RouteDecision;
struct LocalSlotOwnership;
class SlotOwnershipTable;
class SlotMigrationStateMachine;
}

namespace command {

constexpr std::uint16_t kCanonicalCommandVersion = 1;

enum class CommandId : std::uint16_t {
    kSet = 1,
    kExpire = 2,
    kPersist = 3,
    kHSet = 4,
    kLPush = 5,
    kRPush = 6,
    kLPop = 7,
    kRPop = 8,
    kLRem = 9,
    kLTrim = 10,
    kSAdd = 11,
    kSRem = 12,
    kZAdd = 13,
    kZRem = 14,
    kDel = 15,
};

enum class ArgumentType : std::uint8_t {
    kBytes = 1,
    kInt64 = 2,
    kFloat64 = 3,
};

// Bytes are always length-delimited. Numeric values are already parsed and
// normalized before the command can enter Raft.
struct CanonicalArgument {
    ArgumentType type = ArgumentType::kBytes;
    std::string bytes;
    std::int64_t integer = 0;
    std::uint64_t float_bits = 0;

    static CanonicalArgument bytesValue(std::string value);
    static CanonicalArgument integerValue(std::int64_t value);
    static CanonicalArgument floatValue(double value);
    double asFloat() const;
};

struct CanonicalCommand {
    std::uint16_t version = kCanonicalCommandVersion;
    CommandId id = CommandId::kSet;
    cluster::SlotId slot = 0;
    cluster::ShardId shard = cluster::kNoShard;
    std::uint64_t slot_epoch = 0;
    // A leader-supplied, replicated logical instant. Apply never reads wall time.
    UnixMillis logical_time_ms = 0;
    std::vector<CanonicalArgument> arguments;
};

struct CanonicalizeContext {
    cluster::ShardId shard = cluster::kNoShard;
    std::uint64_t slot_epoch = 0;
    UnixMillis logical_time_ms = 0;
};

struct CanonicalizeResult {
    bool ok = false;
    CanonicalCommand command;
    std::string error;
};

// Validates command name, arity, syntax, integer/float ranges and KeySpec.
// Only write commands are accepted because only writes enter the data Raft.
CanonicalizeResult canonicalizeWrite(const std::vector<std::string>& request,
                                     const CanonicalizeContext& context);
CanonicalizeResult canonicalizeWrite(const std::vector<std::string>& request,
                                     const cluster::RouteDecision& route,
                                     UnixMillis logical_time_ms);
CanonicalizeResult canonicalizeWrite(const std::vector<std::string>& request,
                                     const cluster::RouteDecision& route,
                                     UnixMillis logical_time_ms,
                                     const cluster::SlotOwnershipTable& ownership);

// Every key the command writes to. All of them share one slot, because
// canonicalization rejects cross-slot writes.
std::vector<std::string> commandKeys(const CanonicalCommand& command);

// Stable, endian-independent binary format suitable for LogEntry::payload.
std::string encodeCanonicalCommand(const CanonicalCommand& command);
bool decodeCanonicalCommand(const std::string& encoded, CanonicalCommand& command,
                            std::string& error);

struct SlotOwnership {
    cluster::ShardId shard = cluster::kNoShard;
    std::uint64_t epoch = 0;
};

enum class ApplyStatus {
    kApplied,
    kNoOpStaleEpoch,
    kInvalidLog,
    kWrongType,
};

// The response is structured state-machine output, never a pre-encoded RESP frame.
struct ApplyResult {
    ApplyStatus status = ApplyStatus::kInvalidLog;
    long long integer = 0;
    bool has_integer = false;
    std::string bytes;
    bool has_bytes = false;
    std::string error;
};

class DeterministicStateMachine {
public:
    // Marks the database as replicated, which disables its wall-clock-driven
    // background expiry; see Database::markReplicated().
    DeterministicStateMachine(Database& database, cluster::ShardId shard);

    // While a slot is migrating out of this shard, the recorder turns each
    // applied write into a post-image delta. It runs on every replica at the
    // same point in the log, so the delta journal is replicated state and not a
    // leader-local side effect.
    void setMigrationRecorder(cluster::SlotMigrationStateMachine* recorder) {
        migration_ = recorder;
    }

    ApplyResult apply(const std::string& log_payload,
                      const SlotOwnership& current_ownership);
    ApplyResult apply(const CanonicalCommand& command,
                      const SlotOwnership& current_ownership);
    ApplyResult apply(const std::string& log_payload,
                      const cluster::LocalSlotOwnership& current_ownership);
    ApplyResult apply(const CanonicalCommand& command,
                      const cluster::LocalSlotOwnership& current_ownership);
    // Looks the slot's committed ownership up itself, which is what an applier
    // driving a whole Raft group wants.
    ApplyResult apply(const std::string& log_payload,
                      const cluster::SlotOwnershipTable& ownership);

private:
    ApplyResult execute(const CanonicalCommand& command,
                        const SlotOwnership& current_ownership);

    Database& database_;
    cluster::ShardId shard_;
    cluster::SlotMigrationStateMachine* migration_ = nullptr;
};

} // namespace command
