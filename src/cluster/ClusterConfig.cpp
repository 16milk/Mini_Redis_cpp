#include "mini_redis/cluster/ClusterConfig.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cluster {
namespace {

struct ParseError : std::invalid_argument {
    ParseError(std::size_t line, const std::string& what)
        : std::invalid_argument("cluster config line " + std::to_string(line) + ": " +
                                what) {}
};

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        if (!token.empty() && token.front() == '#') {
            break;
        }
        tokens.push_back(token);
    }
    return tokens;
}

unsigned long long parseUnsigned(const std::string& text, unsigned long long limit,
                                 std::size_t line, const char* what) {
    if (text.empty()) {
        throw ParseError(line, std::string("missing ") + what);
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw ParseError(line, std::string(what) + " must be a non-negative integer");
        }
    }
    unsigned long long value = 0;
    for (const char character : text) {
        const auto digit = static_cast<unsigned long long>(character - '0');
        if (value > (limit - digit) / 10ULL) {
            throw ParseError(line, std::string(what) + " is out of range");
        }
        value = value * 10ULL + digit;
    }
    if (value > limit) {
        throw ParseError(line, std::string(what) + " is out of range");
    }
    return value;
}

SlotRange parseSlotRange(const std::string& text, std::size_t line) {
    const std::size_t dash = text.find('-');
    SlotRange range;
    if (dash == std::string::npos) {
        const auto slot = static_cast<SlotId>(
            parseUnsigned(text, kSlotCount - 1, line, "slot"));
        range.start = slot;
        range.end = slot;
        return range;
    }
    range.start = static_cast<SlotId>(
        parseUnsigned(text.substr(0, dash), kSlotCount - 1, line, "slot range start"));
    range.end = static_cast<SlotId>(
        parseUnsigned(text.substr(dash + 1), kSlotCount - 1, line, "slot range end"));
    if (range.start > range.end) {
        throw ParseError(line, "slot range start is greater than end");
    }
    return range;
}

} // namespace

ClusterConfig parseClusterConfig(const std::string& text, std::int64_t now_ms,
                                 const std::string& self_override) {
    NodeId self_id = self_override;
    std::uint64_t epoch = 0;
    std::vector<std::pair<std::size_t, std::vector<std::string>>> nodes;
    std::vector<std::pair<std::size_t, std::vector<std::string>>> shards;
    std::vector<std::pair<std::size_t, std::vector<std::string>>> slots;
    std::vector<std::pair<std::size_t, std::vector<std::string>>> leaders;
    std::vector<std::pair<std::size_t, std::vector<std::string>>> migrations;
    bool auto_slots = false;

    std::istringstream stream(text);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }
        const std::string& directive = tokens.front();

        if (directive == "self") {
            if (tokens.size() != 2) {
                throw ParseError(line_number, "usage: self <node-id>");
            }
            if (self_override.empty()) {
                self_id = tokens[1];
            }
        } else if (directive == "epoch") {
            if (tokens.size() != 2) {
                throw ParseError(line_number, "usage: epoch <config-epoch>");
            }
            epoch = parseUnsigned(tokens[1], std::numeric_limits<std::uint64_t>::max(),
                                  line_number, "config epoch");
        } else if (directive == "node") {
            nodes.emplace_back(line_number, tokens);
        } else if (directive == "shard") {
            shards.emplace_back(line_number, tokens);
        } else if (directive == "slots") {
            slots.emplace_back(line_number, tokens);
        } else if (directive == "auto-slots") {
            if (tokens.size() != 1) {
                throw ParseError(line_number, "usage: auto-slots");
            }
            auto_slots = true;
        } else if (directive == "leader") {
            leaders.emplace_back(line_number, tokens);
        } else if (directive == "migrating") {
            migrations.emplace_back(line_number, tokens);
        } else {
            throw ParseError(line_number, "unknown directive '" + directive + "'");
        }
    }

    if (self_id.empty()) {
        throw std::invalid_argument(
            "cluster config needs a 'self <node-id>' line or an explicit self override");
    }
    if (auto_slots && !slots.empty()) {
        throw std::invalid_argument(
            "cluster config uses both 'auto-slots' and explicit 'slots' lines");
    }

    TopologyBuilder builder;
    builder.setConfigEpoch(epoch);

    for (const auto& [number, tokens] : nodes) {
        if (tokens.size() != 4) {
            throw ParseError(number, "usage: node <node-id> <client-host> <client-port>");
        }
        const auto port = static_cast<std::uint16_t>(
            parseUnsigned(tokens[3], std::numeric_limits<std::uint16_t>::max(), number,
                          "client port"));
        if (port == 0) {
            throw ParseError(number, "client port must not be 0");
        }
        builder.addNode(tokens[1], tokens[2], port);
    }

    std::vector<ShardId> declared_shards;
    for (const auto& [number, tokens] : shards) {
        if (tokens.size() < 3) {
            throw ParseError(number, "usage: shard <shard-id> <voter-node-id>...");
        }
        const auto shard_id = static_cast<ShardId>(
            parseUnsigned(tokens[1], std::numeric_limits<ShardId>::max(), number,
                          "shard id"));
        if (shard_id == kNoShard) {
            throw ParseError(number, "shard id 0 is reserved for 'unassigned'");
        }
        builder.addShard(shard_id,
                         std::vector<NodeId>(tokens.begin() + 2, tokens.end()));
        declared_shards.push_back(shard_id);
    }
    if (declared_shards.empty()) {
        throw std::invalid_argument("cluster config declares no shard");
    }

    if (auto_slots) {
        // 16384 个 slot 均分给 N 个分片：Raft group 数量等于 N，而不是 16384。
        const std::vector<SlotRange> ranges = splitSlotsEvenly(declared_shards.size());
        for (std::size_t index = 0; index < declared_shards.size(); ++index) {
            builder.assignSlots(declared_shards[index], ranges[index]);
        }
    }
    for (const auto& [number, tokens] : slots) {
        if (tokens.size() < 3) {
            throw ParseError(number, "usage: slots <shard-id> <start>-<end> ...");
        }
        const auto shard_id = static_cast<ShardId>(
            parseUnsigned(tokens[1], std::numeric_limits<ShardId>::max(), number,
                          "shard id"));
        for (std::size_t index = 2; index < tokens.size(); ++index) {
            builder.assignSlots(shard_id, parseSlotRange(tokens[index], number));
        }
    }

    for (const auto& [number, tokens] : leaders) {
        if (tokens.size() < 4 || tokens.size() > 5) {
            throw ParseError(number,
                             "usage: leader <shard-id> <node-id> <term> [lease-ms]");
        }
        const auto shard_id = static_cast<ShardId>(
            parseUnsigned(tokens[1], std::numeric_limits<ShardId>::max(), number,
                          "shard id"));
        const auto term = parseUnsigned(
            tokens[3], std::numeric_limits<std::uint64_t>::max(), number, "term");
        std::int64_t expires_at = std::numeric_limits<std::int64_t>::max();
        if (tokens.size() == 5) {
            const auto lease_ms = parseUnsigned(
                tokens[4], static_cast<unsigned long long>(
                               std::numeric_limits<std::int64_t>::max()),
                number, "lease ms");
            if (lease_ms != 0) {
                expires_at = now_ms + static_cast<std::int64_t>(lease_ms);
            }
        }
        builder.setLeaderHint(shard_id, tokens[2], term, expires_at);
    }

    for (const auto& [number, tokens] : migrations) {
        if (tokens.size() < 4 || tokens.size() > 5) {
            throw ParseError(
                number,
                "usage: migrating <slot> <source-shard> <target-shard> [ready|pending]");
        }
        const auto slot = static_cast<SlotId>(
            parseUnsigned(tokens[1], kSlotCount - 1, number, "slot"));
        const auto source = static_cast<ShardId>(
            parseUnsigned(tokens[2], std::numeric_limits<ShardId>::max(), number,
                          "source shard id"));
        const auto target = static_cast<ShardId>(
            parseUnsigned(tokens[3], std::numeric_limits<ShardId>::max(), number,
                          "target shard id"));
        bool target_ready = true;
        if (tokens.size() == 5) {
            if (tokens[4] == "ready") {
                target_ready = true;
            } else if (tokens[4] == "pending") {
                target_ready = false;
            } else {
                throw ParseError(number, "migration state must be 'ready' or 'pending'");
            }
        }
        builder.setMigration(slot, source, target, target_ready);
    }

    ClusterConfig config;
    config.self_id = self_id;
    config.topology = builder.build();
    if (config.topology->findNode(config.self_id) == nullptr) {
        throw std::invalid_argument("cluster config declares self as '" + config.self_id +
                                    "' but has no matching node record");
    }
    return config;
}

ClusterConfig loadClusterConfigFile(const std::string& path, std::int64_t now_ms,
                                    const std::string& self_override) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::invalid_argument("cannot open cluster config file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parseClusterConfig(buffer.str(), now_ms, self_override);
}

} // namespace cluster
