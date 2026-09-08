#pragma once

#include <string>
#include <string_view>
#include <vector>

// 路由所需的命令元数据。集群路由必须在执行前知道"哪些参数是 key"，
// 因此 key 位置不能一律假设为第二个参数。
enum class AccessMode {
    kLocal,  // 无 key，任意健康节点本地执行（PING / ASKING / CLUSTER）
    kRead,   // 读命令，路由到 slot owner 的 leader
    kWrite,  // 写命令，经该 shard 的 Raft 提交
    kAdmin,  // 单节点管理命令，cluster 模式下拒绝
};

enum class KeyLayout {
    kNone,           // 没有 key
    kFirstArgument,  // args[1] 是唯一 key
    kAllArguments,   // args[1..] 全部是 key，例如 DEL / EXISTS
};

struct CommandSpec {
    std::string_view name;
    // Redis 约定：>0 表示参数总数（含命令名）必须精确相等，
    // <0 表示至少需要 -arity 个参数。这里只用于判断 key 是否可提取，
    // 具体的 arity 错误文案仍由各命令自身产生。
    int arity = 0;
    AccessMode access = AccessMode::kLocal;
    KeyLayout key_layout = KeyLayout::kNone;
    bool cluster_supported = true;
};

// 命令名需为大写。未注册的命令返回 nullptr。
const CommandSpec* lookupCommandSpec(const std::string& upper_name);

// 按 spec 提取参与路由的 key。参数个数不足时返回空 vector，
// 让命令自身产生原有的 arity 错误，而不是在路由层换一套文案。
std::vector<std::string_view> extractRoutingKeys(const CommandSpec& spec,
                                                 const std::vector<std::string>& args);
