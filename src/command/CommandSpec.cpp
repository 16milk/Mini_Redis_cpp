#include "mini_redis/command/CommandSpec.hpp"

#include <unordered_map>

namespace {

using Table = std::unordered_map<std::string, CommandSpec>;

CommandSpec makeSpec(std::string_view name, int arity, AccessMode access,
                     KeyLayout layout, bool cluster_supported = true) {
    CommandSpec spec;
    spec.name = name;
    spec.arity = arity;
    spec.access = access;
    spec.key_layout = layout;
    spec.cluster_supported = cluster_supported;
    return spec;
}

const Table& commandTable() {
    static const Table table = [] {
        Table entries;
        const auto add = [&entries](const CommandSpec& spec) {
            entries.emplace(std::string(spec.name), spec);
        };

        // 无 key，本地执行，不进入 Raft。
        add(makeSpec("PING", -1, AccessMode::kLocal, KeyLayout::kNone));
        add(makeSpec("ASKING", 1, AccessMode::kLocal, KeyLayout::kNone));
        add(makeSpec("CLUSTER", -2, AccessMode::kLocal, KeyLayout::kNone));

        // 单 key 写命令。
        add(makeSpec("SET", -3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("EXPIRE", 3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("PEXPIRE", 3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("PERSIST", 2, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("HSET", -4, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("LPUSH", -3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("RPUSH", -3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("LPOP", 2, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("RPOP", 2, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("LREM", 4, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("LTRIM", 4, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("SADD", -3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("SREM", -3, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("ZADD", -4, AccessMode::kWrite, KeyLayout::kFirstArgument));
        add(makeSpec("ZREM", -3, AccessMode::kWrite, KeyLayout::kFirstArgument));

        // 单 key 读命令。
        add(makeSpec("GET", 2, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("TTL", 2, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("PTTL", 2, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("HGET", 3, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("LINDEX", 3, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("LRANGE", 4, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("LLEN", 2, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("SISMEMBER", 3, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("SMEMBERS", 2, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("SCARD", 2, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("ZSCORE", 3, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("ZRANGE", -4, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("ZRANGEBYSCORE", -4, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("ZRANK", 3, AccessMode::kRead, KeyLayout::kFirstArgument));
        add(makeSpec("ZCARD", 2, AccessMode::kRead, KeyLayout::kFirstArgument));

        // 多 key 命令：所有 key 必须同 slot，否则 CROSSSLOT。
        add(makeSpec("DEL", -2, AccessMode::kWrite, KeyLayout::kAllArguments));
        add(makeSpec("EXISTS", -2, AccessMode::kRead, KeyLayout::kAllArguments));

        // 集群模式下没有全局一致时刻，或需要认证的管理入口，首版明确拒绝。
        add(makeSpec("KEYS", 2, AccessMode::kAdmin, KeyLayout::kNone, false));
        add(makeSpec("SAVE", 1, AccessMode::kAdmin, KeyLayout::kNone, false));

        return entries;
    }();
    return table;
}

bool hasEnoughArguments(const CommandSpec& spec, std::size_t argc) {
    if (spec.arity >= 0) {
        return argc == static_cast<std::size_t>(spec.arity);
    }
    return argc >= static_cast<std::size_t>(-spec.arity);
}

} // namespace

const CommandSpec* lookupCommandSpec(const std::string& upper_name) {
    const Table& table = commandTable();
    const auto it = table.find(upper_name);
    return it == table.end() ? nullptr : &it->second;
}

std::vector<std::string_view> extractRoutingKeys(const CommandSpec& spec,
                                                 const std::vector<std::string>& args) {
    std::vector<std::string_view> keys;
    if (spec.key_layout == KeyLayout::kNone || !hasEnoughArguments(spec, args.size())) {
        return keys;
    }

    switch (spec.key_layout) {
        case KeyLayout::kFirstArgument:
            if (args.size() >= 2) {
                keys.emplace_back(args[1]);
            }
            break;
        case KeyLayout::kAllArguments:
            keys.reserve(args.size() - 1);
            for (std::size_t index = 1; index < args.size(); ++index) {
                keys.emplace_back(args[index]);
            }
            break;
        case KeyLayout::kNone:
            break;
    }
    return keys;
}
