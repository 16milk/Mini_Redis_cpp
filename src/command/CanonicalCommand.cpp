#include "mini_redis/command/CanonicalCommand.hpp"

#include "mini_redis/cluster/Router.hpp"
#include "mini_redis/cluster/Slot.hpp"
#include "mini_redis/command/CommandSpec.hpp"
#include "mini_redis/core/Database.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace command {
namespace {

constexpr char kMagic[] = {'M', 'R', 'C', 'C'};
constexpr std::size_t kMaxArguments = 1U << 20;
constexpr std::size_t kMaxEncodedBytes = 64U << 20;

std::string upperAscii(std::string value) {
    for (char& character : value) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return value;
}

bool validArity(const CommandSpec& spec, std::size_t argc) {
    return spec.arity >= 0 ? argc == static_cast<std::size_t>(spec.arity)
                           : argc >= static_cast<std::size_t>(-spec.arity);
}

bool parseInt64(std::string_view input, std::int64_t& value) {
    if (input.empty()) {
        return false;
    }
    bool positive_sign = input.front() == '+';
    if (positive_sign) {
        input.remove_prefix(1);
        if (input.empty()) {
            return false;
        }
    }
    const char* begin = input.data();
    const char* end = begin + input.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parseFiniteDouble(std::string_view input, double& value) {
    if (input.empty()) {
        return false;
    }
    const char* begin = input.data();
    const char* end = begin + input.size();
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value)) {
        return false;
    }
    if (value == 0.0) {
        value = 0.0; // Canonicalize -0 to the all-zero IEEE-754 representation.
    }
    return true;
}

bool checkedAdd(UnixMillis left, UnixMillis right, UnixMillis& result) {
    if ((right > 0 && left > std::numeric_limits<UnixMillis>::max() - right) ||
        (right < 0 && left < std::numeric_limits<UnixMillis>::min() - right)) {
        return false;
    }
    result = left + right;
    return result >= 0;
}

bool secondsToMillis(std::int64_t seconds, std::int64_t& milliseconds) {
    constexpr std::int64_t kMillis = 1000;
    if (seconds > std::numeric_limits<std::int64_t>::max() / kMillis ||
        seconds < std::numeric_limits<std::int64_t>::min() / kMillis) {
        return false;
    }
    milliseconds = seconds * kMillis;
    return true;
}

CanonicalizeResult failure(std::string error) {
    CanonicalizeResult result;
    result.error = std::move(error);
    return result;
}

void addBytes(CanonicalCommand& command, const std::string& value) {
    command.arguments.push_back(CanonicalArgument::bytesValue(value));
}

void addInteger(CanonicalCommand& command, std::int64_t value) {
    command.arguments.push_back(CanonicalArgument::integerValue(value));
}

void appendUnsigned(std::string& out, std::uint64_t value, unsigned width) {
    for (unsigned shift = width * 8; shift != 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> (shift - 8)) & 0xffU));
    }
}

bool readUnsigned(std::string_view input, std::size_t& offset, unsigned width,
                  std::uint64_t& value) {
    if (width > input.size() - offset) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < width; ++index) {
        value = (value << 8) |
                static_cast<unsigned char>(input[offset++]);
    }
    return true;
}

bool bytesAt(const CanonicalCommand& command, std::size_t index) {
    return index < command.arguments.size() &&
           command.arguments[index].type == ArgumentType::kBytes;
}

bool integerAt(const CanonicalCommand& command, std::size_t index) {
    return index < command.arguments.size() &&
           command.arguments[index].type == ArgumentType::kInt64;
}

bool floatAt(const CanonicalCommand& command, std::size_t index) {
    return index < command.arguments.size() &&
           command.arguments[index].type == ArgumentType::kFloat64 &&
           std::isfinite(command.arguments[index].asFloat());
}

bool validateShape(const CanonicalCommand& command, std::string& error) {
    if (command.slot >= cluster::kSlotCount) {
        error = "canonical command slot is out of range";
        return false;
    }
    if (command.logical_time_ms < 0) {
        error = "canonical command logical time is invalid";
        return false;
    }
    const std::size_t count = command.arguments.size();
    bool valid = false;
    switch (command.id) {
        case CommandId::kSet:
            valid = (count == 2 || count == 3) && bytesAt(command, 0) &&
                    bytesAt(command, 1) && (count == 2 || integerAt(command, 2));
            break;
        case CommandId::kExpire:
            valid = count == 2 && bytesAt(command, 0) && integerAt(command, 1);
            break;
        case CommandId::kPersist:
        case CommandId::kLPop:
        case CommandId::kRPop:
            valid = count == 1 && bytesAt(command, 0);
            break;
        case CommandId::kHSet:
            valid = count >= 3 && count % 2 == 1 && bytesAt(command, 0);
            for (std::size_t i = 1; valid && i < count; ++i) valid = bytesAt(command, i);
            break;
        case CommandId::kLPush:
        case CommandId::kRPush:
        case CommandId::kSAdd:
        case CommandId::kSRem:
        case CommandId::kZRem:
            valid = count >= 2;
            for (std::size_t i = 0; valid && i < count; ++i) valid = bytesAt(command, i);
            break;
        case CommandId::kLRem:
            valid = count == 3 && bytesAt(command, 0) && integerAt(command, 1) &&
                    bytesAt(command, 2);
            break;
        case CommandId::kLTrim:
            valid = count == 3 && bytesAt(command, 0) && integerAt(command, 1) &&
                    integerAt(command, 2);
            break;
        case CommandId::kZAdd:
            valid = count >= 3 && count % 2 == 1 && bytesAt(command, 0);
            for (std::size_t i = 1; valid && i < count; i += 2) {
                valid = floatAt(command, i) && bytesAt(command, i + 1);
            }
            break;
        case CommandId::kDel:
            valid = count >= 1;
            for (std::size_t i = 0; valid && i < count; ++i) valid = bytesAt(command, i);
            break;
        default:
            valid = false;
    }
    if (!valid) {
        error = "canonical command argument shape is invalid";
        return false;
    }
    if (cluster::keyToSlot(command.arguments.front().bytes) != command.slot) {
        error = "canonical command KeySpec/slot mismatch";
        return false;
    }
    if (command.id == CommandId::kDel) {
        for (const CanonicalArgument& argument : command.arguments) {
            if (cluster::keyToSlot(argument.bytes) != command.slot) {
                error = "canonical command contains cross-slot keys";
                return false;
            }
        }
    }
    return true;
}

std::vector<std::string> byteTail(const CanonicalCommand& command, std::size_t start) {
    std::vector<std::string> result;
    result.reserve(command.arguments.size() - start);
    for (std::size_t index = start; index < command.arguments.size(); ++index) {
        result.push_back(command.arguments[index].bytes);
    }
    return result;
}

ApplyResult appliedInteger(long long value) {
    ApplyResult result;
    result.status = ApplyStatus::kApplied;
    result.integer = value;
    result.has_integer = true;
    return result;
}

ApplyResult applied() {
    ApplyResult result;
    result.status = ApplyStatus::kApplied;
    return result;
}

} // namespace

CanonicalArgument CanonicalArgument::bytesValue(std::string value) {
    CanonicalArgument argument;
    argument.type = ArgumentType::kBytes;
    argument.bytes = std::move(value);
    return argument;
}

CanonicalArgument CanonicalArgument::integerValue(std::int64_t value) {
    CanonicalArgument argument;
    argument.type = ArgumentType::kInt64;
    argument.integer = value;
    return argument;
}

CanonicalArgument CanonicalArgument::floatValue(double value) {
    CanonicalArgument argument;
    argument.type = ArgumentType::kFloat64;
    if (value == 0.0) value = 0.0;
    std::memcpy(&argument.float_bits, &value, sizeof(value));
    return argument;
}

double CanonicalArgument::asFloat() const {
    double value = 0;
    std::memcpy(&value, &float_bits, sizeof(value));
    return value;
}

CanonicalizeResult canonicalizeWrite(const std::vector<std::string>& request,
                                     const CanonicalizeContext& context) {
    if (request.empty()) return failure("empty command");
    const std::string name = upperAscii(request[0]);
    const CommandSpec* spec = lookupCommandSpec(name);
    if (spec == nullptr) return failure("unknown command");
    if (!validArity(*spec, request.size())) return failure("wrong number of arguments");
    if (spec->access != AccessMode::kWrite) return failure("command is not a Raft write");
    if (context.shard == cluster::kNoShard || context.slot_epoch == 0) {
        return failure("missing slot ownership context");
    }
    if (context.logical_time_ms < 0) {
        return failure("invalid logical time");
    }

    const std::vector<std::string_view> keys = extractRoutingKeys(*spec, request);
    if (keys.empty()) return failure("write command has no key");
    const cluster::SlotId slot = cluster::keyToSlot(keys.front());
    for (const std::string_view key : keys) {
        if (cluster::keyToSlot(key) != slot) return failure("CROSSSLOT");
    }

    CanonicalCommand command;
    command.slot = slot;
    command.shard = context.shard;
    command.slot_epoch = context.slot_epoch;
    command.logical_time_ms = context.logical_time_ms;

    if (name == "SET") {
        if (request.size() != 3 && request.size() != 5) return failure("syntax error");
        command.id = CommandId::kSet;
        addBytes(command, request[1]);
        addBytes(command, request[2]);
        if (request.size() == 5) {
            const std::string option = upperAscii(request[3]);
            if (option != "EX" && option != "PX") return failure("syntax error");
            std::int64_t ttl = 0;
            if (!parseInt64(request[4], ttl)) return failure("invalid integer");
            if (ttl <= 0) return failure("invalid expire time");
            if (option == "EX" && !secondsToMillis(ttl, ttl)) {
                return failure("invalid expire time");
            }
            UnixMillis deadline = 0;
            if (!checkedAdd(context.logical_time_ms, ttl, deadline)) {
                return failure("invalid expire time");
            }
            addInteger(command, deadline);
        }
    } else if (name == "EXPIRE" || name == "PEXPIRE") {
        command.id = CommandId::kExpire;
        addBytes(command, request[1]);
        std::int64_t ttl = 0;
        if (!parseInt64(request[2], ttl)) return failure("invalid integer");
        if (name == "EXPIRE" && !secondsToMillis(ttl, ttl)) {
            return failure("invalid expire time");
        }
        UnixMillis deadline = context.logical_time_ms;
        if (ttl > 0 && !checkedAdd(context.logical_time_ms, ttl, deadline)) {
            return failure("invalid expire time");
        }
        addInteger(command, deadline);
    } else if (name == "PERSIST") {
        command.id = CommandId::kPersist;
        addBytes(command, request[1]);
    } else if (name == "HSET") {
        if ((request.size() - 2) % 2 != 0) return failure("wrong number of arguments");
        command.id = CommandId::kHSet;
        for (std::size_t i = 1; i < request.size(); ++i) addBytes(command, request[i]);
    } else if (name == "LPUSH" || name == "RPUSH" || name == "SADD" ||
               name == "SREM" || name == "ZREM" || name == "DEL") {
        if (name == "LPUSH") command.id = CommandId::kLPush;
        if (name == "RPUSH") command.id = CommandId::kRPush;
        if (name == "SADD") command.id = CommandId::kSAdd;
        if (name == "SREM") command.id = CommandId::kSRem;
        if (name == "ZREM") command.id = CommandId::kZRem;
        if (name == "DEL") command.id = CommandId::kDel;
        for (std::size_t i = 1; i < request.size(); ++i) addBytes(command, request[i]);
    } else if (name == "LPOP" || name == "RPOP") {
        command.id = name == "LPOP" ? CommandId::kLPop : CommandId::kRPop;
        addBytes(command, request[1]);
    } else if (name == "LREM") {
        command.id = CommandId::kLRem;
        addBytes(command, request[1]);
        std::int64_t count = 0;
        if (!parseInt64(request[2], count)) return failure("invalid integer");
        addInteger(command, count);
        addBytes(command, request[3]);
    } else if (name == "LTRIM") {
        command.id = CommandId::kLTrim;
        addBytes(command, request[1]);
        std::int64_t start = 0;
        std::int64_t stop = 0;
        if (!parseInt64(request[2], start) || !parseInt64(request[3], stop)) {
            return failure("invalid integer");
        }
        addInteger(command, start);
        addInteger(command, stop);
    } else if (name == "ZADD") {
        if (request.size() % 2 != 0) return failure("wrong number of arguments");
        command.id = CommandId::kZAdd;
        addBytes(command, request[1]);
        for (std::size_t i = 2; i < request.size(); i += 2) {
            double score = 0;
            if (!parseFiniteDouble(request[i], score)) return failure("invalid float");
            command.arguments.push_back(CanonicalArgument::floatValue(score));
            addBytes(command, request[i + 1]);
        }
    } else {
        return failure("write command has no canonical form");
    }

    CanonicalizeResult result;
    result.ok = true;
    result.command = std::move(command);
    return result;
}

CanonicalizeResult canonicalizeWrite(const std::vector<std::string>& request,
                                     const cluster::RouteDecision& route,
                                     UnixMillis logical_time_ms) {
    if (route.action != cluster::RouteAction::kLocal || !route.has_slot ||
        route.shard == cluster::kNoShard || route.slot_epoch == 0) {
        return failure("request has no local slot ownership");
    }
    CanonicalizeContext context;
    context.shard = route.shard;
    context.slot_epoch = route.slot_epoch;
    context.logical_time_ms = logical_time_ms;
    CanonicalizeResult result = canonicalizeWrite(request, context);
    if (result.ok && result.command.slot != route.slot) {
        return failure("routing KeySpec does not match canonical command");
    }
    return result;
}

std::string encodeCanonicalCommand(const CanonicalCommand& command) {
    std::string error;
    if (command.version != kCanonicalCommandVersion ||
        command.shard == cluster::kNoShard || command.slot_epoch == 0 ||
        !validateShape(command, error)) {
        throw std::invalid_argument(error.empty() ? "invalid canonical command" : error);
    }
    std::string out;
    out.reserve(36);
    out.append(kMagic, sizeof(kMagic));
    appendUnsigned(out, command.version, 2);
    appendUnsigned(out, static_cast<std::uint16_t>(command.id), 2);
    appendUnsigned(out, command.slot, 2);
    appendUnsigned(out, command.shard, 4);
    appendUnsigned(out, command.slot_epoch, 8);
    appendUnsigned(out, static_cast<std::uint64_t>(command.logical_time_ms), 8);
    appendUnsigned(out, command.arguments.size(), 4);
    for (const CanonicalArgument& argument : command.arguments) {
        out.push_back(static_cast<char>(argument.type));
        if (argument.type == ArgumentType::kBytes) {
            if (argument.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("canonical argument is too large");
            }
            appendUnsigned(out, argument.bytes.size(), 4);
            out.append(argument.bytes);
        } else if (argument.type == ArgumentType::kInt64) {
            appendUnsigned(out, static_cast<std::uint64_t>(argument.integer), 8);
        } else {
            appendUnsigned(out, argument.float_bits, 8);
        }
        if (out.size() > kMaxEncodedBytes) {
            throw std::length_error("canonical command is too large");
        }
    }
    return out;
}

bool decodeCanonicalCommand(const std::string& encoded, CanonicalCommand& command,
                            std::string& error) {
    if (encoded.size() > kMaxEncodedBytes ||
        encoded.size() < sizeof(kMagic) ||
        std::memcmp(encoded.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "invalid canonical command header";
        return false;
    }
    std::size_t offset = sizeof(kMagic);
    std::uint64_t value = 0;
    CanonicalCommand decoded;
    if (!readUnsigned(encoded, offset, 2, value)) return false;
    decoded.version = static_cast<std::uint16_t>(value);
    if (decoded.version != kCanonicalCommandVersion) {
        error = "unsupported canonical command version";
        return false;
    }
    if (!readUnsigned(encoded, offset, 2, value)) return false;
    decoded.id = static_cast<CommandId>(value);
    if (!readUnsigned(encoded, offset, 2, value)) return false;
    decoded.slot = static_cast<cluster::SlotId>(value);
    if (decoded.slot >= cluster::kSlotCount) {
        error = "canonical command slot is out of range";
        return false;
    }
    if (!readUnsigned(encoded, offset, 4, value)) return false;
    decoded.shard = static_cast<cluster::ShardId>(value);
    if (!readUnsigned(encoded, offset, 8, decoded.slot_epoch)) return false;
    if (!readUnsigned(encoded, offset, 8, value)) return false;
    decoded.logical_time_ms = static_cast<std::int64_t>(value);
    if (!readUnsigned(encoded, offset, 4, value) || value > kMaxArguments) {
        error = "invalid canonical argument count";
        return false;
    }
    const std::size_t argument_count = static_cast<std::size_t>(value);
    decoded.arguments.reserve(argument_count);
    for (std::size_t index = 0; index < argument_count; ++index) {
        if (offset == encoded.size()) {
            error = "truncated canonical argument";
            return false;
        }
        CanonicalArgument argument;
        argument.type = static_cast<ArgumentType>(
            static_cast<unsigned char>(encoded[offset++]));
        if (argument.type == ArgumentType::kBytes) {
            std::uint64_t length = 0;
            if (!readUnsigned(encoded, offset, 4, length) ||
                length > encoded.size() - offset) {
                error = "truncated canonical byte argument";
                return false;
            }
            argument.bytes.assign(encoded.data() + offset,
                                  static_cast<std::size_t>(length));
            offset += static_cast<std::size_t>(length);
        } else if (argument.type == ArgumentType::kInt64) {
            if (!readUnsigned(encoded, offset, 8, value)) return false;
            argument.integer = static_cast<std::int64_t>(value);
        } else if (argument.type == ArgumentType::kFloat64) {
            if (!readUnsigned(encoded, offset, 8, argument.float_bits)) return false;
            const double number = argument.asFloat();
            if (!std::isfinite(number) || (number == 0.0 && argument.float_bits != 0)) {
                error = "non-canonical float argument";
                return false;
            }
        } else {
            error = "unknown canonical argument type";
            return false;
        }
        decoded.arguments.push_back(std::move(argument));
    }
    if (offset != encoded.size() || decoded.shard == cluster::kNoShard ||
        decoded.slot_epoch == 0 || !validateShape(decoded, error)) {
        if (error.empty()) error = "trailing or invalid canonical command data";
        return false;
    }
    command = std::move(decoded);
    error.clear();
    return true;
}

ApplyResult DeterministicStateMachine::apply(
    const std::string& log_payload, const SlotOwnership& current_ownership) {
    CanonicalCommand command;
    std::string error;
    if (!decodeCanonicalCommand(log_payload, command, error)) {
        ApplyResult result;
        result.error = std::move(error);
        return result;
    }
    return apply(command, current_ownership);
}

ApplyResult DeterministicStateMachine::apply(
    const CanonicalCommand& command, const SlotOwnership& current_ownership) {
    std::string error;
    if (command.version != kCanonicalCommandVersion ||
        command.slot >= cluster::kSlotCount || !validateShape(command, error)) {
        ApplyResult result;
        result.error = error.empty() ? "invalid canonical command" : std::move(error);
        return result;
    }
    if (command.shard != shard_ || current_ownership.shard != shard_ ||
        command.slot_epoch != current_ownership.epoch) {
        ApplyResult result;
        result.status = ApplyStatus::kNoOpStaleEpoch;
        return result;
    }

    Database::ScopedLogicalTime logical_time(database_, command.logical_time_ms);
    try {
        const auto& args = command.arguments;
        switch (command.id) {
            case CommandId::kSet:
                if (args.size() == 2) {
                    database_.set(args[0].bytes, args[1].bytes);
                } else {
                    const UnixMillis deadline = args[2].integer;
                    if (deadline <= command.logical_time_ms) {
                        return ApplyResult{ApplyStatus::kInvalidLog, 0, false, {}, false,
                                           "SET deadline is not in the future"};
                    }
                    database_.set(args[0].bytes, args[1].bytes,
                                  deadline - command.logical_time_ms);
                }
                return applied();
            case CommandId::kExpire: {
                const UnixMillis deadline = args[1].integer;
                const UnixMillis ttl = deadline <= command.logical_time_ms
                                           ? 0
                                           : deadline - command.logical_time_ms;
                return appliedInteger(database_.expire(args[0].bytes, ttl) ? 1 : 0);
            }
            case CommandId::kPersist:
                return appliedInteger(database_.persist(args[0].bytes) ? 1 : 0);
            case CommandId::kHSet: {
                std::vector<std::pair<std::string, std::string>> values;
                values.reserve((args.size() - 1) / 2);
                for (std::size_t i = 1; i < args.size(); i += 2) {
                    values.emplace_back(args[i].bytes, args[i + 1].bytes);
                }
                return appliedInteger(static_cast<long long>(
                    database_.hset(args[0].bytes, values)));
            }
            case CommandId::kLPush:
                return appliedInteger(static_cast<long long>(
                    database_.lpush(args[0].bytes, byteTail(command, 1))));
            case CommandId::kRPush:
                return appliedInteger(static_cast<long long>(
                    database_.rpush(args[0].bytes, byteTail(command, 1))));
            case CommandId::kLPop:
            case CommandId::kRPop: {
                ApplyResult result;
                result.status = ApplyStatus::kApplied;
                result.has_bytes = command.id == CommandId::kLPop
                                       ? database_.lpop(args[0].bytes, result.bytes)
                                       : database_.rpop(args[0].bytes, result.bytes);
                return result;
            }
            case CommandId::kLRem:
                return appliedInteger(database_.lrem(
                    args[0].bytes, args[1].integer, args[2].bytes));
            case CommandId::kLTrim:
                database_.ltrim(args[0].bytes, args[1].integer, args[2].integer);
                return applied();
            case CommandId::kSAdd:
                return appliedInteger(static_cast<long long>(
                    database_.sadd(args[0].bytes, byteTail(command, 1))));
            case CommandId::kSRem:
                return appliedInteger(static_cast<long long>(
                    database_.srem(args[0].bytes, byteTail(command, 1))));
            case CommandId::kZAdd: {
                std::vector<std::pair<double, std::string>> values;
                values.reserve((args.size() - 1) / 2);
                for (std::size_t i = 1; i < args.size(); i += 2) {
                    values.emplace_back(args[i].asFloat(), args[i + 1].bytes);
                }
                return appliedInteger(static_cast<long long>(
                    database_.zadd(args[0].bytes, values)));
            }
            case CommandId::kZRem:
                return appliedInteger(static_cast<long long>(
                    database_.zrem(args[0].bytes, byteTail(command, 1))));
            case CommandId::kDel:
                return appliedInteger(static_cast<long long>(
                    database_.del(byteTail(command, 0))));
        }
    } catch (const std::runtime_error&) {
        ApplyResult result;
        result.status = ApplyStatus::kWrongType;
        result.error = "WRONGTYPE";
        return result;
    }
    ApplyResult result;
    result.error = "unknown canonical command";
    return result;
}

} // namespace command
