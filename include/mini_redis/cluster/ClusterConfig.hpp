#pragma once

#include "mini_redis/cluster/Topology.hpp"

#include <cstdint>
#include <string>

namespace cluster {

struct ClusterConfig {
    NodeId self_id;
    TopologyPtr topology;
};

// 解析集群描述文本。在完成形态里这些记录来自已提交的 metadata Raft group；
// 这里用一个文件把同样的输入喂给 TopologyBuilder，好处是路由层的代码路径
// 与真实部署完全一致，只是快照的来源不同。
//
// 支持的指令（'#' 起始为注释）：
//   self       <node-id>
//   epoch      <config-epoch>
//   node       <node-id> <client-host> <client-port>
//   shard      <shard-id> <voter-node-id>...
//   slots      <shard-id> <start>-<end> | <slot> ...
//   auto-slots
//   leader     <shard-id> <node-id> <term> [lease-ms]
//   migrating  <slot> <source-shard> <target-shard> [ready|pending]
//
// `auto-slots` 把 16384 个 slot 按声明顺序均分给所有 shard，用来演示
// "slot 只是路由单元，Raft group 数量等于分片数量"。
// `lease-ms` 省略或为 0 时 leader hint 不过期，适用于静态配置；
// 真实部署由带 term 的心跳周期性刷新。
//
// `self_override` 非空时优先于文件里的 `self` 行，这样同一份拓扑描述
// 可以被同集群的多个节点共用。
ClusterConfig parseClusterConfig(const std::string& text, std::int64_t now_ms,
                                 const std::string& self_override = std::string());

ClusterConfig loadClusterConfigFile(const std::string& path, std::int64_t now_ms,
                                    const std::string& self_override = std::string());

} // namespace cluster
