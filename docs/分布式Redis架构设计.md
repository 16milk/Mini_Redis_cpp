# Mini_Redis_cpp 分布式架构设计

> 设计日期：2026-08-25
>
> 文档状态：设计提案，本文描述的集群、复制、WAL、控制面和在线迁移均尚未实现
>
> 基线版本：`b4b7b0a`
>
> 目标定位：教学型、可验证的分布式 Redis 子集，不宣称与官方 Redis 完全兼容或生产就绪

## 1. 执行摘要

本文建议将当前单机 `Mini_Redis_cpp` 演进为一套 **Redis Cluster 风格路由 + 每逻辑分片一个 Raft 复制组** 的分布式系统：

- key 使用与 Redis Cluster 一致的 16384 个 hash slot 和 hash tag 规则；
- 16384 个 slot 映射到少量逻辑分片，而不是创建 16384 个 Raft group；
- 每个逻辑分片默认由 3 个副本组成，写入经 Raft 多数派持久化、提交并应用后才返回成功；
- 默认读由 leader 使用 `ReadIndex` 提供分片内线性一致性；
- 独立的 3 或 5 副本元数据 Raft group 管理节点、分片、slot 和迁移状态，但不进入普通数据请求热路径；
- 客户端直接连接数据节点，节点使用 `MOVED`、`ASK` 和 `CROSSSLOT` 引导 cluster-aware 客户端，不做透明代理；
- RDB 保留为 Redis 兼容的导入导出格式；正式崩溃恢复使用分片 WAL 与带 Raft index 的一致性快照；
- 在线迁移采用“快照 + 增量追赶 + 源端 fence + 目标激活”的持久状态机，不使用客户端双写；
- 同 slot 多 key 写命令由一个 Raft entry 原子执行，读命令在同一个 ReadIndex barrier 后读取；跨 slot 命令首版直接拒绝，不引入跨分片 2PC。

该方案选择 CP：发生网络分区时，只有持有多数派的分片可以继续确认写入。系统宁可让部分 slot 暂时不可用，也不能让两个 owner 同时接受写入。

## 2. 当前基线与改造前提

当前项目的实际请求链路是：

```text
TCP + RESP
    -> Server 单线程 LT epoll Reactor
    -> Connection read_buffer_
    -> RespParser
    -> CommandHandler::execute
    -> 单个 Database
    -> RedisObject
    -> RESP response
```

已有的可复用基础包括：

- RESP 数组请求解析、常用响应编码、pipeline 和部分网络读写；
- String、Hash、List、Set、ZSet 五类对象；
- Hash 的紧凑编码、哈希表和渐进式 rehash；
- Redis 风格 RDB 的五类对象编码、CRC64、临时文件写入、`fsync` 和原子 `rename`；
- 协议、命令语义、对象编码、RDB 和 `socketpair` I/O 测试。

分布式化前必须正视以下边界：

1. `main.cpp` 中的全局 `g_db` 才是命令实际操作的数据库，但 `Server` 自己还持有另一份未使用的 `Database db_`。两者都会尝试加载 `dump.rdb`，状态所有权不唯一。
2. `CommandHandler` 使用长 `if/else` 分派，没有命令读写属性、key 提取规则、确定性属性或跨 slot 规则。
3. `Database` 只是一个 `unordered_map<string, shared_ptr<RedisObject>>`，没有 slot、版本、TTL、日志 index、快照边界或并发保护。
4. 当前没有 AOF/WAL、复制 offset、选举、节点身份、成员发现和故障转移。
5. RDB 是同步全量快照，直接遍历活动可变对象；它不能直接充当 Raft snapshot。
6. RDB 加载损坏时会以空库继续启动。分布式节点必须改为 fail-closed，不能把数据损坏解释成合法空状态。
7. 正常命令由单线程隐式串行化；任何直接增加的后台复制或快照线程都会与 map、对象编码、Hash rehash 和 ZSet 双索引发生数据竞争。
8. 运行态尚无 TTL。加载器虽然会跳过已经过期的 RDB key，但不会保留未来过期时间。
9. 当前信号处理器直接执行日志、RDB 保存和 `exit`，不是 async-signal-safe。
10. 输入、输出、连接数、pipeline 和命令执行均没有资源上限或公平预算。

因此，实施顺序必须先建立“命令元数据 -> 路由 -> 共识提案 -> 确定性状态机”的边界，再增加真正的多节点能力。不能让网络层、迁移器或复制线程绕过状态机直接修改 `Database`。

## 3. 目标、非目标与故障模型

### 3.1 目标

- 兼容 Redis Cluster 的 slot、hash tag、`MOVED`、`ASK`、`ASKING` 和 `CROSSSLOT` 核心语义。
- 数据可按逻辑分片水平扩展，一个进程可承载多个分片副本。
- 每个分片在多数副本可用时提供自动选主和分片内线性一致读写。
- 正常的单节点宕机不丢失已经返回成功的写入。
- slot 可以在线迁移，迁移中断、节点重启或 leader 切换后能够幂等恢复。
- WAL、snapshot、内部 RPC 和元数据均有版本、校验、长度限制和明确的损坏处理。
- 尽量复用当前对象层、RESP 层和 RDB 对象编码能力。
- 各阶段均能独立测试和运行，避免一次性重写整个仓库；代码回退只保证在尚未启用不可逆持久格式或迁移 fence 前可行。

### 3.2 非目标

- 首版不实现 Redis 的全部命令、RESP3、Lua、Pub/Sub、Stream、ACL 或模块系统。
- 首版不实现跨 slot 事务、跨分片原子命令或透明 2PC。
- 不实现 Redis `PSYNC` 异步主从协议；数据副本使用 Raft。
- 不把 16384 个 slot 各自做成一个 Raft group。
- 不保证不同分片之间存在全局顺序或全局线性一致性。
- 不在第一轮 Raft 实现中同时引入 TTL、事务和多线程对象锁。
- 不处理 Byzantine 节点；假设节点可能宕机、重启、变慢、丢包、乱序、分区或磁盘报错，但不会恶意伪造协议。
- 不承诺跨地域低延迟；首版目标是单地域、多个故障域部署。
- 不把本文视为生产就绪声明。认证、加密、配额、升级和灾难恢复完成前，只适合受控环境。

### 3.3 一致性与可用性承诺

| 场景 | 语义 |
| --- | --- |
| 单 key 读写 | 默认在所属分片内线性一致 |
| 同 slot 多 key 命令 | 写命令一个 Raft entry 原子 apply；读命令在一个 ReadIndex barrier 后读取同一 applied 状态 |
| 跨 slot 命令 | 返回 `CROSSSLOT` |
| follower 读取 | 默认关闭；显式开启时仅承诺 stale read |
| leader 故障 | 多数副本存活时重新选主，短暂不可用 |
| 少数派网络分区 | 不确认写，也不提供默认强一致读 |
| 分片失去多数派 | 对应 slot 不可用，其他分片继续服务 |
| 控制面失去多数派 | 已稳定分片继续服务；禁止拓扑、成员和迁移变更 |
| 客户端超时 | 结果未知；写可能已经提交，基础 RESP 模式不保证 exactly-once |
| 已确认写的持久性 | 严格 fsync 模式下，完整多数派未永久丢失时 RPO 为 0 |

“RPO 为 0”只覆盖本文故障模型。若一个分片的多数磁盘永久丢失、管理员强制重建 quorum 或存储设备违反持久化语义，仍可能丢失数据。

## 4. 核心术语

| 术语 | 含义 |
| --- | --- |
| Node | 一个 `mini_redis_server` 进程实例，具有永久 `node_id` |
| Slot | Redis Cluster 风格的 0..16383 路由单元 |
| Shard / Group | 拥有若干 slot 的逻辑数据分片，同时也是一个 Raft group |
| Replica | 某个 group 在一个 Node 上的本地副本 |
| Voter | 参与 Raft 投票和 quorum 的副本 |
| Learner | 只追赶日志、不计入 quorum 的新副本 |
| Metadata Group | 保存集群拓扑和迁移编排状态的独立 Raft group |
| Topology Revision | 元数据状态每次提交后的单调版本，用于缓存更新 |
| Ownership Epoch | 某个 slot 所有权世代，用于拒绝旧 owner、旧迁移和延迟消息 |
| Commit Index | Raft 已被多数派确认的最大日志 index |
| Applied Index | 本地状态机已经应用的最大日志 index |
| Fence | 源分片持久化停止某个 slot 新写的状态机记录 |

一个逻辑 shard 可以拥有连续或不连续的多个 slot。slot 数固定为 16384，但 shard 数量由部署规模决定，例如小型集群可从 4 到 16 个 shard 起步。最终数量必须由数据量、热点、WAL/fsync 能力和调度开销决定。

## 5. 架构决策

| 问题 | 决策 | 原因 |
| --- | --- | --- |
| 分片方式 | 16384 slots + Redis hash tag | 兼容成熟客户端路由模型，迁移粒度稳定 |
| 复制方式 | 每个逻辑 shard 一个 Raft group | 已确认写不依赖异步主从窗口，故障语义可证明 |
| slot 与 Raft group 关系 | 多个 slot 归属一个 group | 避免 16384 个共识组的定时器、WAL 和调度成本 |
| 客户端路由 | 服务端返回重定向，不透明代理 | 避免额外网络跳和代理故障域，贴近 Redis Cluster |
| 默认读 | Leader + ReadIndex | 不依赖时钟 lease，语义清晰 |
| 控制面 | 独立 3/5 副本 Raft group | 拓扑变更强一致，但不让控制面进入普通数据热路径 |
| 多 key | 只允许同 slot | 保留单组原子性，避免首版引入 2PC |
| 正式恢复 | WAL + 分片 snapshot | RDB 不包含 term/index/成员和迁移状态 |
| RDB 定位 | 导入、导出、兼容与离线备份材料 | 继续利用现有实现，但不承担共识日志职责 |
| 分片内执行 | 单 actor 串行 apply | 复用当前无锁对象，实现确定性的命令原子性 |
| 在线迁移 | 单写 owner、源端 fence、目标端激活 | 避免客户端双写产生不确定结果和双主 |

### 5.1 与官方 Redis Cluster 的兼容边界

本文兼容的是客户端可见的路由模型，不复刻官方 Redis Cluster 的内部实现：

| 能力 | 目标兼容性 |
| --- | --- |
| RESP2 与当前已实现命令 | 保持兼容子集 |
| 16384 slots、CRC16、hash tag | 精确兼容 |
| `MOVED`、`ASK`、`ASKING`、`CROSSSLOT` | 精确兼容核心语义 |
| `CLUSTER SLOTS` | 首版实现兼容矩阵所需的 RESP2 子集 |
| `CLUSTER SHARDS` | 后续能力；响应格式测试完成前不宣称支持 |
| Redis Cluster bus/gossip | 不兼容；使用独立版本化内部 RPC |
| `CLUSTER MEET`、官方故障转移流程 | 不兼容；使用 Raft 元数据控制面和管理 API |
| `PSYNC`、replication offset/backlog | 不兼容；使用 Raft WAL、snapshot 和 `InstallSnapshot` |
| 官方 Redis 的异步复制数据丢失窗口 | 不保留；本文选择多数派持久化后确认的 CP 语义 |

因此，普通 Redis 客户端可以接入已实现的命令子集；cluster-aware 客户端只有列入 20.7 节兼容矩阵且通过指定版本测试后才宣称受支持。官方 Redis 节点不能作为本集群的数据副本或控制面成员，`redis-cli --cluster` 的全部运维命令也不在首版兼容范围内。

## 6. 必须长期成立的安全不变量

以下规则是实现契约，而不是优化建议：

1. 任意时刻，一个 slot 最多只能由一个分片确认写，不论请求携带哪个 epoch；epoch 用于 fence 旧 owner，不能让新旧 owner 并行写。
2. 所有用户数据写入、过期删除、迁移导入和迁移清理都必须通过所属 Raft group 的 committed log。
3. 网络线程、管理线程、快照线程和迁移线程不得直接修改 `ShardStore`。
4. 每个 group 的活动状态机严格按 log index 单线程应用，同一状态世代内每个 index 最多应用一次；崩溃恢复可以从较早 snapshot 重建新状态并确定性 replay。
5. Leader 只有在 entry 被多数 voter 持久化、提交并由本地状态机应用后，才向客户端返回写结果。
6. 默认读必须确认当前 leader 仍拥有 quorum，并等待 `last_applied >= read_index`。
7. 同一个 slot 的多 key 写命令要么作为一个 entry 完整执行，要么完全不执行；多 key 读在同一个 ReadIndex barrier 后读取同一 applied 状态。
8. topology revision 只能单调前进；ownership epoch 不得回退或复用。两者含义不同，不能混为一个计数器。
9. 源分片一旦提交 fence，即使控制面失联或节点重启，也不得自行恢复该 slot 写入。
10. 旧 epoch 的请求、日志转发、迁移 chunk 和管理命令必须被拒绝或确定性地变成 no-op。
11. snapshot 必须对应一个明确的 `lastIncludedIndex/Term`，后台编码不得读取仍在原地变化的对象。
12. snapshot 真正 durable 之前不得删除它覆盖的 WAL；恢复失败不得静默启动为空库。
13. Raft 成员变更只能通过 joint consensus；不得直接从旧 voter 集合跳到新集合。
14. 同一客户端连接的 RESP 响应必须保持请求顺序，即使请求在不同 group 异步完成。
15. 任何磁盘 `fsync`、校验或身份错误都不能只记录日志后继续确认写。
16. 新 epoch 的目标激活必须以旧 owner 已提交的 fence 为前置条件，不能只凭控制面修改 slot 映射。

## 7. 目标架构

### 7.1 集群视图

```text
                         cluster-aware clients
                                  |
                         RESP / client port
                                  |
              +-------------------+-------------------+
              |                   |                   |
         Data Node A         Data Node B         Data Node C
       replicas: G1,G3     replicas: G1,G2     replicas: G1,G2,G3
              |                   |                   |
              +--------- versioned internal RPC -----+
                                  |
              +-------------------+-------------------+
              |                                       |
       Data Raft Groups                        Metadata Raft Group
       G1 / G2 / G3 ...                 nodes, groups, slots, migrations
```

控制面和数据面是逻辑角色。小型开发环境可以让同一二进制同时承载两种角色，但它们必须使用独立的 Raft group、状态目录、RPC 服务和资源配额。正式部署中，控制面 voter 应分布在不同故障域，不能因为进程共置而把 3 副本退化成一个故障点。

### 7.2 数据节点内部

```text
DataNode
├── ClientReactor                 TCP、RESP、连接会话、背压
├── ClusterRouter                 KeySpec、slot、拓扑与重定向
├── RaftTransport                 节点间 RPC、连接池、优先队列
├── GroupScheduler                固定线程池，保证每个 group 串行执行
│   ├── ShardReplica G1
│   │   ├── RaftCore
│   │   ├── WAL / Snapshot
│   │   └── ShardStateMachine -> ShardStore
│   └── ShardReplica G2 ...
├── SnapshotWorkers               只消费不可变 snapshot
├── MetadataWatcher               发布不可变 TopologySnapshot
└── AdminServer                   独立认证、独立端口
```

不为每个 group 创建常驻线程。`GroupScheduler` 使用固定工作线程和 group mailbox；同一 group 的任务永不并发执行，不同 group 可以并行。这样能保留当前对象容器无需内部锁的性质，并避免 shard 数增加时线程数失控。

客户端连接属于创建它的 Reactor。Raft 完成结果通过线程安全完成队列返回原 Reactor，再由连接级序号按顺序写回。

### 7.3 `ShardStore`

目标存储抽象为：

```cpp
class ShardStore {
public:
    ApplyResult apply(const CanonicalCommand& command);
    ReadResult read(const CanonicalCommand& command) const;
    ShardSnapshot snapshot(uint64_t applied_index) const;

private:
    std::unordered_map<std::string, ObjectEntry> values_;
    std::array<std::unordered_set<std::string>, 16384> slot_keys_; // 概念结构
    SlotOwnershipTable ownership_;
};
```

实际实现不必为每个空 slot 分配一个 `unordered_set`，可以使用稀疏 map 或按 group 本地 slot 建索引。关键约束是：

- 一个 shard 只有一个 key 目录；
- 每次创建、覆盖和删除 key 时同步维护 `slot -> keys` 索引；
- slot 索引只用于迁移、扫描和校验，不作为第二份对象所有权；
- Hash 的 rehash 状态、ZSet 的两个索引等仍以一个 Redis key 为不可分割对象；
- 所有对象访问都经过 `ShardStore`，不得暴露可变裸指针供后台任务修改。

`slot_keys_` 是派生索引。snapshot 中可以保存它以加速恢复，但加载后必须能从 `values_` 重建并校验一致性。

## 8. Slot、KeySpec 与客户端路由

### 8.1 Slot 算法

必须精确实现 Redis Cluster 规则：

```text
slot = CRC16_XMODEM(key_material) & 0x3fff
```

`key_material` 的选择规则：

1. 查找第一个 `{`；
2. 从它后面查找第一个 `}`；
3. 如果两者之间至少有一个字节，只 hash 中间内容；
4. 如果没有右括号，或第一个匹配是空 `{}`，hash 完整 key；不会继续寻找后面的另一组括号。

例如 `{user:42}:profile` 与 `{user:42}:orders` 必须落在同一 slot。key 按原始字节和显式长度计算，允许包含 `\0`，不能使用 C 字符串函数、locale 或 `std::hash`。CRC 实现必须用官方兼容测试向量验证。

### 8.2 命令注册表

现有 `CommandHandler::execute` 的长分支应替换为注册表。每个 `CommandSpec` 至少声明：

```cpp
struct CommandSpec {
    CommandId id;
    std::string_view name;
    ArityRule arity;
    AccessMode access;             // LOCAL / READ / WRITE / ADMIN
    KeyExtractor extract_keys;
    bool deterministic;
    bool allow_stale_follower_read;
    MigrationPolicy migration_policy;
    uint16_t state_machine_version;
};
```

key 位置不能一律假设为第二个参数：

- `GET key`、`HSET key field value` 只有一个路由 key；
- `DEL key [key ...]` 和 `EXISTS key [key ...]` 的所有后续参数都是 key；
- `PING`、`ASKING` 没有 key；
- 将来的脚本或可变参数命令需要专用 key 提取器。

### 8.3 请求路由流程

```text
RESP parse
  -> CommandSpec/arity/静态参数校验
  -> 提取所有 key
  -> 计算并校验同一 slot
  -> 查询不可变 TopologySnapshot
  -> 校验本地 SlotOwnership + epoch
  -> READ: ReadIndex 后读状态机
     WRITE: propose 到 Raft
  -> 编码 RESP 结果
```

节点行为：

- 无 key 的 `PING` 可在任意健康节点本地执行；
- slot 的当前 leader 在本机时正常处理；
- slot 属于其他 group 或 leader 在其他节点时返回 `-MOVED <slot> <host:port>`；
- slot 处于切换窗口、目标已经安全激活且源 Raft 已提交 `TargetReady(proof)` 时，源返回 `-ASK <slot> <host:port>`；
- 拓扑暂时无法判定时返回 `-TRYAGAIN ...`；
- 集群尚未形成、slot 未覆盖或本地状态不足以证明安全时返回 `-CLUSTERDOWN ...`；
- 数据节点不替客户端跨节点代理普通命令。

当前 `RespParser::encodeError()` 会固定添加 `-ERR`，无法生成 Redis 客户端可识别的重定向。协议层必须增加 typed error encoder，直接生成 `-MOVED`、`-ASK`、`-CROSSSLOT`、`-TRYAGAIN` 和 `-CLUSTERDOWN`。

### 8.4 多 key 与特殊命令

| 命令类别 | Cluster mode 策略 |
| --- | --- |
| `GET/HGET/LINDEX/...` | 路由到唯一 key 的 slot，默认 leader 强一致读 |
| `SET/HSET/LPUSH/LPOP/SADD/ZADD/...` | 路由到唯一 key 的 group，经 Raft 写入 |
| `DEL/EXISTS key...` | 所有 key 同 slot时执行，否则 `CROSSSLOT` |
| `PING` | 本地执行，不进入 Raft |
| `KEYS *` | 首版 cluster mode 禁用；后续提供按分片 `SCAN` |
| `SAVE` | 首版 cluster mode 禁用；由认证的管理 API 触发 group snapshot/export |
| `ASKING` | 修改连接的一次性路由状态，不进入数据 Raft |
| `CLUSTER SLOTS` | 从已提交元数据与当前可信 leader hint 合成 |
| `CLUSTER SHARDS` | 首版未实现 |

同 slot 的 `DEL` 必须作为一条日志记录整体应用，不能拆成多个独立提案。`EXISTS` 是读命令，同样只允许同 slot，以避免一次请求跨多个一致性域。

`KEYS *` 在集群里既没有自然的全局时刻，又会长时间阻塞分片，因此首版明确拒绝。后续 `SCAN` 返回 cursor 时要把 group、slot epoch 和局部 cursor 编码进去；拓扑变化时允许重复，不允许静默漏掉属于同一稳定 epoch 的 key。

### 8.5 `ASKING` 会话语义

`ASKING` 是连接级、一次性的标志：

- 收到 `ASKING` 后仅允许紧随其后的一个已解析命令访问 importing slot；
- 无论下一个命令成功、失败或没有 key，都清除该标志；
- 标志按连接请求序号绑定，不能因跨 group 异步完成而误用于其他请求；
- 连接断开后状态消失。

因此 `Connection` 需要显式的 `ClientSession`，而不再只是 fd 与两个字符串缓冲区。

目标节点只有在本地 Raft 已提交并 apply 对应 slot/E+1 的 `ACTIVE_ASK` 状态时，才消费该一次性标志。ASK 读仍执行目标 group 的 ReadIndex，ASK 写仍经目标 Raft；单独发送 `ASKING` 不能获得任何 slot 的访问权。

### 8.6 拓扑响应与 leader hint

Metadata group 保存稳定成员、slot owner group 和 config index，不把瞬时 data leader 写入每次 topology revision。每个 data group leader 通过带 term 和短有效期的已认证 heartbeat 发布 `LeaderHint(group_id, leader_node_id, term)`；节点只接受当前 group 成员且 term 不低于已知 term 的 hint。

`CLUSTER SLOTS`/`CLUSTER SHARDS` 由 committed metadata 与可信 hint 合成：primary endpoint 必须是当前已知 leader 的 client advertise address，replica 随后列出。leader 未知、hint 过期或地址未在 committed node record 中时，命令返回 `TRYAGAIN leader unknown`，不返回半张或带非标准占位符的 slot 表。leader 变化不必提交 metadata，但必须使节点缓存失效并能被下一次拓扑查询观察到；客户端命中旧 leader 时，旧节点只在掌握可信新 hint 时返回指向新 leader 的 `MOVED`，未知时返回 `TRYAGAIN`，不得形成互相指向的环。

首版只以 RESP2 `CLUSTER SLOTS` 为兼容入口。每个 slot range 返回：`[start, end, primary, replica...]`，每个 node tuple 固定为 `[host bulk-string, port integer, node-id bulk-string]`；primary 必须是当前 leader。host 使用非空的 client advertise IPv4、IPv6 或兼容矩阵允许的 hostname；IPv6 重定向采用 `[addr]:port`。`CLUSTER SHARDS`、空地址和 Redis 7 hostname 扩展推迟到各自格式测试完成后，不能仅凭本设计宣称支持。

## 9. 命令状态机设计

### 9.1 分层

命令执行拆为四层：

1. **Parse/Validate**：RESP 解码、arity、整数/浮点格式和请求大小等静态校验。
2. **Route**：KeySpec、slot、topology、leader 与 migration 状态判断。
3. **Propose/Read Barrier**：写入提交 Raft；强一致读获取 ReadIndex。
4. **Apply/Read**：只在 group actor 中访问 `ShardStore`，生成结构化结果。

RESP 编码属于最外层，不进入状态机。Raft 日志保存版本化的规范命令，不保存客户端原始帧，也不保存 RESP 响应字符串。

### 9.2 规范日志载荷

概念格式如下：

```text
CanonicalCommand
  codec_version       u16
  command_id          u16
  state_machine_ver   u16
  slot                u16
  ownership_epoch     u64
  flags               u32
  args_count          u32
  args                repeated length-prefixed bytes
  client_id           optional 128-bit
  client_sequence     optional u64
```

Raft 的 term、index 和 entry type 位于日志 envelope，不重复放在命令 payload 中。所有长度必须先校验上限和整数溢出。字符串保持二进制安全；整数在 proposal 前转为规范有符号整数；ZSet score 转为确定的 IEEE-754 64 位编码，拒绝 NaN 和无穷值，并把 `-0` 规范化为 `+0`。

静态错误在 proposal 前返回。依赖当前数据的结果，例如 `WRONGTYPE`、`HSET` 新增字段数或 `LPOP` 返回值，必须在 apply 时按日志顺序决定。这样不会因为 leader 预检查和实际提交之间插入其他 entry 而产生不一致。

### 9.3 确定性要求

所有副本对同一日志前缀必须得到相同逻辑状态：

- apply 期间禁止读取墙上时间、随机数、网络、本地文件或未复制配置；
- 不得把指针、`size_t`、本机字节序结构体或 `std::hash` 结果写入持久格式；
- `unordered_map`、`unordered_set` 的遍历顺序不得影响写结果、snapshot 或 state hash；
- snapshot 和测试 state hash 对 key、Hash field、Set member、ZSet `(score, member)` 做规范排序；
- 非确定性命令若未来引入，由 leader 先物化成确定的 mutation，再写日志；
- 后台 rehash 只可改变内部表示，不得影响逻辑结果；snapshot 编码逻辑内容而非 bucket 布局；
- 状态机格式升级必须声明版本，rolling upgrade 期间只能产生所有 voter 都能理解的最低兼容版本。

已经 committed 的 entry 不能因为某个副本本地 OOM、磁盘错误或对象损坏而被当作普通命令错误跳过。所有复合 mutation 必须提供强异常安全：先完成可能失败的分配与新状态构造，再原子发布，不能留下半个 `HSET`、部分 `DEL` 或只更新一半的 ZSet 索引。若 apply 遭遇无法恢复的本地环境错误，该 replica 应进入隔离/PANIC 状态并从健康副本恢复，不能返回一个仅本机产生的结果后继续参与 quorum。

状态机 apply 不执行外部副作用。重启时允许从 durable snapshot 之后重新 replay 同一 index 来重建易失内存，但在一次有效状态世代内不能重复作用；任何需要跨重试去重的用户语义必须进入复制状态，而不能依赖“apply 函数只被调用一次”。

### 9.4 响应顺序与 pipeline

请求进入连接时分配递增 `request_seq`。不同 group 可以并发完成，但 Reactor 只发送从 `next_response_seq` 开始的连续结果。待发送和待完成响应均有硬上限；超过高水位时暂停该连接 `EPOLLIN`，下降到低水位后恢复。

同一 group 的请求按进入 group mailbox 的顺序执行。不同 group 没有全局执行顺序；非 pipeline 客户端通过“等待上一条响应再发送下一条”自然建立因果关系。跨 shard pipeline 只保证响应顺序，不承诺各 shard 的实际执行先后。

### 9.5 超时、重试与去重

基础 RESP 没有 request ID。若写入已经提交但响应丢失，客户端重试 `LPUSH`、`LPOP` 等非幂等命令可能重复执行。因此首版明确采用：

- 单次连接内不主动重试用户写；
- 客户端超时表示 outcome unknown；
- 客户端若重试，承担至少一次调用语义；
- 推荐业务为需要安全重试的写设计幂等 key 或比较条件。

增强模式可增加 `client_id + monotonically increasing sequence`。去重表必须作为状态机状态复制、进入 snapshot，并有每 client 有界窗口；只在 leader 内存中缓存不能跨故障转移提供 exactly-once。该能力不属于首版 Redis 兼容协议。

## 10. 每分片 Raft 设计

### 10.1 Group 与角色

每个逻辑 shard 对应一个 Raft group，默认 3 个 voter，并尽量分布在不同机器和故障域。一个 Node 可以承载多个 group 的 replica，但同一个 group 在同一 Node 上最多一个 replica。

副本角色包括 follower、candidate、leader 和 learner。`leader_hint` 只是加速路由的信息，不是所有权证明；真正的领导权由当前 term、日志和 quorum 决定。

### 10.2 写路径

```text
客户端写命令
  -> 静态校验 / slot / epoch
  -> group leader append 本地 WAL
  -> AppendEntries 到 followers
  -> 多数 voter durable ack
  -> 推进 commitIndex
  -> group actor 顺序 apply
  -> 返回 apply result
```

关键约束：

- follower 只有在对应 entry 已写入稳定存储后才能成功响应 `AppendEntries`；
- leader 自身同样先持久化，再计入多数派；
- `currentTerm/votedFor` 必须在发送投票成功响应或依赖新 term 的消息前持久化；Raft core 输出的 durable work、outbound message 和 apply work 必须遵守先持久化再发送/应用的顺序；
- leader 只能用当前 term 的 entry 推进 commit，旧 term entry 通过提交当前 term entry 间接变为 committed；
- 客户端成功响应晚于本地 apply，因此返回值与可见状态对应；
- proposal 在排队期间若遇到 slot fence 或 epoch 变化，apply 必须按状态机规则确定性拒绝/no-op；
- WAL 可以 group commit，但不能为了吞吐绕过 durability gate。

### 10.3 默认强一致读

首版不用 leader lease，采用 `ReadIndex`：

1. leader 上任后先确保已经提交当前 term 的一条 entry，通常是每个 leadership 只需一次的 no-op/barrier；
2. leader 向 quorum 确认自己仍是 leader；
3. 获得 `read_index`；
4. 等待本地 `last_applied >= read_index`；
5. 再在 group actor 中读取。

普通 ReadIndex 不追加日志、不执行 `fsync`，也不要求对象级版本号；它只做 quorum read barrier 并等待本地状态机追上。只有新 leader 尚未提交当前 term entry 时才需要先完成上述一次性 barrier。这样不依赖机器墙上时钟和最大暂停时间。后续若用 lease 优化，必须明确时钟漂移、进程暂停、续租和失效边界，并保留 ReadIndex 回退。

显式 stale follower read 只读取本地 applied 状态，不保证 read-your-writes、单调读或最新 topology；响应与指标应暴露 applied index/lag。默认客户端不启用它。

### 10.4 选举与 leader 退位

- 使用 PreVote，减少隔离节点恢复后无意义地抬高 term；
- 选举超时随机化，心跳和选举计时使用 monotonic clock；
- 启用 CheckQuorum，失去多数派联系的旧 leader 主动停止确认读写；
- 支持 `TimeoutNow`/leader transfer，计划下线 leader 前先迁移领导权；
- event loop stall、磁盘 stall 和内部 RPC 饥饿必须有指标，否则容易被误判为网络故障；
- 内部 Raft 心跳、投票和 append 流量拥有独立连接池和优先级，不能被慢客户端耗尽。

### 10.5 成员变更

成员变更采用 joint consensus：

1. 新副本以 learner 加入；
2. 安装 snapshot 并追赶至允许的 lag；
3. 提交 `old + new` 联合配置；
4. 再提交最终 `new` 配置；
5. 一次只执行一个配置变更。

联合配置不是 voter 并集的普通多数：joint entry 的提交必须同时得到 old configuration 的多数和 new configuration 的多数；joint entry committed 并 apply 后，后续 entry 才按联合 quorum 判断；final new configuration committed 并 apply 后，才能退出联合规则。learner 未追平不得提升为 voter；删除当前 leader 前先 transfer。控制面记录期望配置和操作 request ID，但不能仅修改 metadata 就替代数据 group 自己的 joint consensus。

失去 quorum 时禁止自动缩小 voter 集合来“恢复服务”，因为这可能让两个不相交集合分别形成多数。灾难恢复必须是显式、带风险确认和审计的人工流程。

## 11. WAL、Snapshot 与恢复

### 11.1 数据目录

建议每个永久节点使用显式绝对路径：

```text
data_dir/
├── identity                         # cluster_id + node_id
├── metadata-cache/                  # 最后已提交 topology snapshot
└── groups/
    └── <group_id>/
        ├── hard-state
        ├── wal/segment-*.wal
        ├── snapshots/<index>-<term>/
        └── tmp/
```

禁止继续依赖进程工作目录下的隐式 `dump.rdb`。`cluster_id`、`node_id` 和 `group_id` 必须在每个持久文件中交叉校验，避免把旧磁盘目录误挂到另一个集群。节点地址可以变化，永久身份不能因重启或 IP 变化重新生成。

### 11.2 WAL

每个 WAL segment/record 至少包含：

- magic、format version、header length；
- cluster ID、group ID；
- term、index、entry type；
- payload length 和 checksum；
- 必要的 feature/version bitmap。

另行持久化 `currentTerm`、`votedFor`、commit/snapshot metadata。写入顺序和原子性必须保证投票与日志不会在重启后倒退。

WAL 使用分段文件和批量 fsync。严格模式下，一批 entry 在本地 `fsync` 完成前不能 ack 给 leader，leader 在多数 durable 前不能确认客户端。若将来提供 `fsync=everysec`，必须明确它降低已确认数据的崩溃持久性，不能继续宣称严格 Raft durability。

恢复时：

- 校验所有 header、identity、长度、index 连续性和 checksum；
- 只允许安全截断最后一个 segment 尾部的不完整 record；
- 中间 record 损坏、index 回退或 identity 不匹配时进入隔离状态并拒绝服务；
- 未确认 committed 的尾日志不得自行 apply；
- `ENOSPC`、`EIO` 或 `fsync` 失败的副本立即停止 durable ack；leader 本地存储失败时停止确认新写并退位/隔离。

### 11.3 分片 Snapshot

Raft snapshot 至少包含：

```text
SnapshotManifest
  format_version
  cluster_id / group_id
  last_included_index / last_included_term
  raft_configuration
  owned_slots + ownership_epoch/state
  migration state and staging data
  optional deduplication table
  object stream metadata
  checksums
```

对象流编码逻辑状态，并按 key/field/member 规范排序。所有整数和浮点使用固定字节序与格式。Hash rehash 的 bucket 布局、容器 capacity 和指针不属于逻辑 snapshot。

创建 snapshot 时，group actor 在明确的 applied index 建立一致视图。v1 采用“actor 内短暂停顿深复制、worker 线程编码落盘”：复制期间该 group 暂停 apply，但其他 group 不停；复制完成后立即恢复 apply，编码和 `fsync` 均在 actor 外执行。启动前按 `store.memory_usage()` 做内存预检，默认要求进程尚有至少 `1.2 * estimated_snapshot_copy_bytes + safety_reserve` 的可用预算；不足则拒绝本轮 snapshot、保留 WAL 并报警，绝不能边复制边耗尽内存。单节点同时只允许一个深复制 snapshot，且测试必须记录 pause p99 和内存峰值。

该 v1 路径优先保证正确性，不能承诺 GB 级分片的低暂停。进入大数据量阶段前，应复用《读写并发、写时复制与锁设计》中的对象级 COW：actor 只复制 key 目录和只读句柄，后续首次写共享 key 时 detach，worker 只读不可变旧版本。绝不能把当前 `shared_ptr` map 浅拷贝后继续原地修改对象。若深复制 pause 或内存门槛不满足服务 SLO，必须先完成 COW，不能用读取活动 map 的后台线程规避问题。

落盘顺序：

1. 在 `tmp/` 写所有 chunk 和 manifest；
2. 校验完整长度与 checksum；
3. `fsync` 文件；
4. 原子激活 snapshot 目录/manifest；
5. `fsync` 父目录；
6. 更新 durable snapshot pointer；
7. 最后才允许压缩被覆盖且不再被迁移 pin 的 WAL。

`InstallSnapshot` 使用有上限的分块传输、offset 和逐块/整体校验。接收方写临时目录，完整验证 cluster/group/term/index 后原子替换；失败时继续保留旧 snapshot 和 WAL。

### 11.4 启动恢复顺序

```text
读取并校验永久 identity
  -> 加载最新完整 snapshot
  -> 恢复 Raft hard state / membership
  -> 恢复 committed index / applied index / ownership metadata
  -> 校验 WAL 连续性并仅重放已提交区间
  -> 与 peers 建立联系并确认角色
  -> 加载持久 topology 与 slot epoch
  -> 才开放客户端端口
```

恢复实现不能在知道 `commitIndex` 前重放任意 WAL entry。snapshot manifest 先给出 `last_included_index/term`、成员配置和 ownership 状态；hard state、commit metadata 与 WAL manifest 再共同决定可重放上界。若 WAL 在 `(last_included_index, commitIndex]` 内有空洞、term 不匹配或 checksum 失败，该 replica 必须拒绝启动并等待 snapshot install/人工恢复，不能跳过损坏 entry 或把未提交 entry apply 到状态机。`commitIndex` 之后的本地 WAL 只作为 Raft 协议日志保留，是否截断由重新加入集群后的 AppendEntries 决定。

恢复过程中客户端端口不开启，或统一返回 `LOADING`；绝不允许先以空状态接受写入再异步恢复。

### 11.5 RDB 的新定位

现有 RDB encoder/decoder 继续用于：

- 离线导入已有 Redis 基础对象；
- 导出某个 group/slot 的 Redis 兼容数据；
- 测试对象序列化兼容性；
- 未来备份系统的数据载荷之一。

RDB 不记录 term、index、成员配置、ownership epoch、迁移状态或 dedup 表，不能单独作为 Raft 恢复点。集群级备份若要得到跨 shard 一致时刻，需要额外的 barrier/backup coordinator；简单地依次对各 shard 执行 `SAVE` 只能得到 fuzzy backup，不能宣称全局一致。

在线导入也不能在某一个 replica 上直接调用 RDB decoder 修改活动 map。首版只允许在空集群、开放客户端端口之前，把 RDB 校验并转换为各 group 的初始 Raft snapshot；未来若支持在线导入，则必须先按 slot 拆分，再通过受控的 Raft entry 或全组一致的 snapshot 安装流程提交。导入时发现不属于目标 group 的 key 必须拒绝，不能静默丢弃。

## 12. 元数据控制面

### 12.1 元数据内容

Metadata Raft group 保存：

```text
ClusterMetadata
├── cluster_id
├── revision
├── nodes[node_id] -> client/internal/admin addresses, failure domain, status
├── groups[group_id] -> voters, learners, desired placement, config index
├── slots[0..16383] -> active_group_id, active_epoch, stable/transition state
└── migrations[migration_id] -> source/target, from/to epoch, phase, progress proof
```

管理命令全部携带幂等 request ID，并以 CAS 形式检查预期 revision/epoch。seed 地址只用于发现，不能充当节点身份。

### 12.2 数据节点消费拓扑

数据节点 watch committed metadata revision，并将其构造成不可变 `TopologySnapshot` 原子发布给路由线程：

- 新 revision 可以替换旧 revision，旧消息不能覆盖新状态；
- leader 地址只是 hint，实际失败时通过 group 状态刷新；
- topology cache 应持久化，以便控制面短暂不可用时恢复稳定分片；
- 接受写入还必须同时满足本地 group 的持久 `SlotOwnership` 与 epoch，不能只相信一张最终一致的 slot 表。

### 12.3 控制面失联

控制面失去 quorum 时：

- 已处于 `STABLE`、且数据 group 自身有 quorum 的 slot 继续读写；
- 禁止新建/删除 group、改变 voter、开始迁移或提交新 owner；
- 已 fence 的源绝不能因为看不到控制面而重新开放；
- 迁移只能根据已持久化阶段安全地暂停或继续 roll-forward；
- 不能把控制面故障扩散为全体数据请求失败。

迁移是例外的局部可用性窗口：若 source 已 fence，source/target 可以依据已提交 intent 和 proof 继续推进到 `TARGET_ACTIVE_ASK`，客户端经原 source 获得 `ASK` 后仍可访问；但没有 Metadata quorum 就不能提交正式 owner，直接命中 target 的普通请求仍不能被当作稳定路由。若 source/target 也无法完成 proof chain，该 slot 会保持 `TRYAGAIN`，且 fence 不允许超时回滚。运维告警必须把这种“安全但可能无限等待外部 quorum 恢复”的状态列为高优先级；恢复手段是恢复控制面或缺失的数据 group quorum 并继续 roll-forward，不是解除 fence。

资源承诺必须与这个安全选择一致：`SOURCE_FENCED` 之后不能为了释放 WAL/staging 而自动回滚，因此系统需要为迁移保留独立配额、节流前台写入，并在达到 `migration_wal_pin_limit` 或 staging 上限时进入 `MIGRATION_BLOCKED` 告警状态。该状态仍保留 source/target 的最小证明链和 slot 数据，拒绝新的迁移和可能扩大资源占用的写入；只有恢复 quorum 并完成 metadata commit 后才能正常 cleanup。若永久丢失 quorum，唯一释放方式是显式灾难恢复流程，由运维确认目标状态、选择继续提交或离线导出/重建，并记录会丢失哪些未证明的可用性语义；设计不能承诺在这种场景下自动无限期保留服务能力又自动释放所有资源。

### 12.4 集群初始化

新集群必须通过显式 bootstrap 操作创建唯一 `cluster_id`，且只允许在空数据目录执行。加入节点需要 bootstrap/join token、现有控制面地址和持久 node identity。禁止多个空节点仅凭互相发现自动形成集群，以免网络分区产生两个同名集群。

### 12.5 阶段 1 的静态 Bootstrap

阶段 1 尚无 Metadata group，使用一次性的、内容完全相同的静态 bootstrap manifest 启动三副本：

```yaml
format_version: 1
cluster_id: <bootstrap tool generated UUID>
group_id: data-0001
slots: [0, 16383]
ownership_epoch: 1
voters:
  - {node_id: <A>, raft: 127.0.0.1:16379, client: 127.0.0.1:6379}
  - {node_id: <B>, raft: 127.0.0.1:16380, client: 127.0.0.1:6380}
  - {node_id: <C>, raft: 127.0.0.1:16381, client: 127.0.0.1:6381}
```

计划中的 `mini_redis_admin init-static` 一次性生成 cluster ID、三个唯一 node ID 和 manifest。每个节点仅在空 `data_dir` 下使用 `--bootstrap-manifest` 初始化 identity、初始 group configuration 和 slot ownership，并把 manifest digest 写入磁盘；正常重启只读取磁盘状态。manifest、磁盘 identity 或只读期望配置不匹配时 fail-closed。阶段 1 不支持在线成员变化；节点替换等到阶段 3 的 learner/joint consensus。

首次启动时还必须显式传入 `--local-node-id <A|B|C>` 和该节点唯一的 `--data-dir`；local node ID 必须在 manifest voter 列表中恰好出现一次，其 listen/advertise 地址必须与本机配置一致。三个进程可以按任意顺序启动，达到多数后选主。重启时禁止再次传 bootstrap 参数，只从原 data dir 恢复；空目录节点不能伪装成已有 node ID 加入。

这一流程只用于开发阶段的固定集群，不与后续 join 协议混用。阶段 2 起，新节点先本地生成并持久化唯一 node ID，再通过受认证 join 注册；cluster ID 只从已有控制面取得，节点不能自行选择或覆盖。

## 13. 在线 Slot 迁移

### 13.1 为什么不使用客户端双写

客户端或源节点同时写源、目标时，可能出现一边成功一边超时、重试重复、两个 leader 先后切换以及写入顺序相反。没有分布式事务就无法判断最终状态。因此迁移期间始终只有一个能够确认新写的 owner。

### 13.2 持久迁移状态机

```text
STABLE
  -> PREPARING
  -> PREPARED
  -> COPYING_BASE
  -> CATCHING_UP
  -> SOURCE_FENCED
  -> TARGET_ACTIVE_ASK
  -> METADATA_COMMITTED
  -> CLEANUP
  -> STABLE
```

详细流程：

1. **CREATE INTENT**：控制面 CAS 创建唯一 `migration_id`，记录 slot、source、target、当前 `from_epoch=E`、预留 `to_epoch=E+1` 和 `PREPARING` 状态，但正式 owner 仍是 source/E，不能提前发布 target/E+1。
2. **PREPARE PARTICIPANTS**：目标先提交 `PrepareImport(migration_id, slot, E, E+1)`，源再提交 `PrepareExport(...)`，最后控制面凭两份 committed proof 把 phase CAS 为 `PREPARED`。每一步都按 migration ID 幂等。跨三个 Raft group 不要求原子提交，因为此阶段 source/E 仍是唯一服务者，target 只有不可见 staging。
3. **BASE SNAPSHOT**：源在 committed/applied index `S` 为该 slot 建一致性快照。源仍是唯一 owner，只接受路由层注入 `E` 的普通读写，同时 pin `S` 之后的 WAL；target staging 只接受迁移协议中的 `E+1`，不接受客户端。
4. **COPY BASE**：快照分成有大小上限和 checksum 的 chunk，通过目标 group 的迁移 entry 复制到 staging namespace。所有 chunk 到齐并校验后，目标必须提交并 apply `BaseComplete(manifest, S, base_state_hash)`；manifest 绑定 migration ID、slot、E/E+1、chunk 清单、总字节数和 base identity。只有该记录生效后才能开始增量追赶。首版让 chunk 经过目标 Raft，虽然写放大较高，但其恢复语义最简单；后续可优化为“所有目标 voter 先 durable 下载 blob，再提交 manifest”。
5. **CATCH UP**：源从 `S+1` 开始扫描 committed log，将每条记录的实际 apply outcome 转为版本化 `MigrationDelta`，而不是在目标重放携带 E 的原始 `CanonicalCommand`。delta 绑定 migration ID、slot、E/E+1 和 source index，描述已物化 mutation、确定性 no-op 或区间 progress；不相关 entry 形成的 index 空洞通过每批 `scan_start/scan_end` 进度显式跨过。目标通过自身 Raft 幂等应用 delta，同时分别记录 `last_slot_mutation_index` 和连续单调的 `source_processed_through`，不能把“最后一条相关 mutation 的 index”误当成连续追赶进度。
6. **FENCE SOURCE**：源提交 `Fence(migration_id, slot, E, E+1)`，停止该 slot 的普通读写，得到最终 source index `F`。`SourceFenceProof` 不是单节点返回的 term/index，而是对 source 当前 leader 发起 ReadIndex 线性一致查询后返回的已提交状态：包含 migration ID、slot、E/E+1、source group、fence entry term/index、`commitIndex >= F`、state hash 和 leader lease/term 观察信息。验证方必须通过认证 RPC 向 source 当前 leader 重新查询或刷新该 proof，不能只相信旧 leader 曾经声称写入过某个 index。若 source 换主或重启，新 leader 从已提交状态恢复 fence；若查询不到已提交 fence，则目标不得激活。fence 之后所有 E 或旧 epoch proposal 必须确定性 no-op/拒绝；在 target ready 前，源对普通请求返回 `TRYAGAIN`。
7. **DRAIN**：目标确认 `source_processed_through == F`，即已检查到 fence entry且不存在遗漏的 slot mutation，再校验 key 数和规范 state hash。
8. **ACTIVATE TARGET**：目标通过 source 当前 leader 的 ReadIndex 线性查询验证 `SourceFenceProof` 后，提交并 apply `Activate(migration_id, slot, E, E+1, F, base_identity, state_hash)`，原子地把 staging 变为活动 slot。若 source 暂无 leader、无法达到 quorum、proof 与本地 drain 结果不一致，目标必须停在 `CATCHING_UP/SOURCE_FENCED` 并继续重试，不能依据缓存 proof 激活。目标 group ID、该 activation entry 的 term/index 及完整内容构成可跨故障验证的 `ActivationProof`；不能拿 target log index 与 source 的 `F` 做数值比较。目标仍只接受带一次性 `ASKING`、且与本地 `ACTIVE_ASK` slot/E+1 匹配的请求；所有 ASK 写进入目标的正常 Raft/WAL/snapshot。迁移从此只能 roll-forward，不能回滚或清空目标状态。
9. **ASK WINDOW**：源从当前 target leader 的线性一致读验证 `ActivationProof` 后，先在源 Raft 提交 `TargetReady(migration_id, slot, E, E+1, F, proof)`，然后才对该 slot 的所有普通读写返回 `ASK`。源 leader 切换后从该记录恢复 ASK 状态，不能依赖易失观察。
10. **COMMIT METADATA**：控制面通过认证 RPC，分别对 source 和 target 当前 leader 做 ReadIndex/线性一致查询，校验相同的 `SourceFenceProof`、`TargetReady` 与 `ActivationProof`，并持久记录 proof tuple，再 CAS 把正式 owner 从 source/E 改为 target/E+1。proof 至少绑定 migration ID、slot、from/to epoch、source group 与 F、base identity/state hash、target group 与 activation term/index。
11. **NORMAL ROUTING**：目标观察到 committed metadata revision 后接受普通请求；源对所有普通读写只返回 `MOVED`，不得在清理前读取旧值。
12. **CLEANUP**：经过安全保留期后，源通过自己的 Raft entry 删除旧 slot 数据并解除 WAL pin。

### 13.3 迁移约束

- 所有步骤以 `migration_id + epoch` 幂等；节点重启后从 committed 状态恢复。
- 普通客户端不携带 epoch；router 从已验证的本地 ownership 状态给 `CanonicalCommand` 注入 epoch。fence 前 source 只接受 E，target staging/`ACTIVE_ASK` 只接受 E+1，metadata transition 同时保存 from/to epoch。
- source WAL 在目标追赶和控制面提交前不能压缩掉所需增量。
- 同 slot 多 key 命令整体属于一个迁移流，不能拆到源和目标。
- target staging 对普通读写不可见；激活必须是一个状态机原子操作。
- 目标接收 chunk、staging 数据和 WAL pin 都有磁盘配额与 backpressure。
- 旧 epoch 的 chunk、重复 source index、跳跃 index 或错误 checksum 必须拒绝。
- WAL pin 和重启后的传输游标以 `source_processed_through` 为准；只有经过覆盖区间验证的 progress marker 才能跨过没有该 slot mutation 的 index 空洞。
- metadata commit 不能单方面创造安全所有权；它必须建立在源已 fence、目标已激活的 durable 证明上。`SourceFenceProof` 的验证动作必须是对 source 当前 leader 的线性一致查询，或未来引入等价的多数派 quorum certificate；v1 不接受单副本签名或旧 leader 缓存响应。
- `ASKING` 本身不携带 epoch 或 migration ID；目标除了检查连接的一次性标志，还必须从本地已提交状态确认该 slot 正处于对应 epoch 的 `ACTIVE_ASK`。
- moving slot 禁止 follower stale read；source fence 后 source follower 也不得从旧副本返回数据，目标 `ACTIVE_ASK` 的读只能经 target leader 的 ReadIndex。
- target 激活后若控制面暂时失去 quorum，系统保持 source fenced、target 仅 ASK 的安全状态；期间的 ASK 写继续进入 target Raft 并被最终 owner 继承，不能回滚、清空 staging 或重新开放 source。若资源上限被触发，进入限流/阻塞和高优先级告警，而不是静默压缩被迁移依赖的 WAL。
- source GC 只影响旧副本数据，不得绕过 Raft 直接删除本地文件。
- `PREPARING/PREPARED` 到提交 fence 之前可以通过控制面 CAS 进入 `ABORTING`，再以幂等 Raft entry 让 source 解除 pin、target 清理 staging，收齐清理 proof 后标记 `ABORTED`；超时协调者只重试这些步骤，不能直接遗忘记录。从 `SOURCE_FENCED` 起只能 roll-forward。

首版按“整个 slot”迁移，不实现 Redis 那种逐 key 已迁移/未迁移混合路由。这样 ASK 窗口更短、协议状态更少，也更容易证明安全。

### 13.4 增量日志与状态摘要

`PrepareExport` 生效后，source 状态机为该 migration 建立有界 `ExportJournal`。每次 apply 都产生确定性的 `MutationBatch` 与逻辑结果：属于目标 slot 的实际 mutation 被保存为 `MigrationDelta`；无 mutation 的日志仍推进连续扫描水位。v1 delta 使用 `PutCanonicalObject(key, version, full logical value)` 或 `DeleteKey(key, version)`，用写放大换取简单、确定的重放；后续只有在证明有收益时才增加版本化字段级 patch。该 journal 是 source 的复制状态，会进入 snapshot，并可由 snapshot + WAL 重建；leader 切换不能丢失。传输确认只回收目标已经通过自身 Raft 持久化的前缀。

同一 slot 同时最多存在一个 migration；单 group 默认最多运行一个 base copy，可配置少量 catch-up 并发。达到 journal、WAL pin 或 staging 上限时，在 fence 前走 replicated abort，在 fence 后对前台写施加背压并优先 roll-forward。

迁移比较使用版本化的 `StateDigestV1`，算法为 SHA-256，输入是带域分隔符的规范字节流：

```text
"mini-redis-state-v1"
slot
for key in bytewise_sorted_keys:
    key_length + key_bytes + logical_type
    canonical_logical_value
    optional_expire_deadline_and_version
```

Hash field、Set member 按原始字节排序；List 保持元素顺序；ZSet 以 member 字节排序并编码规范化的 IEEE-754 score；长度与数值均使用固定大端格式。摘要忽略 ZIPLIST/HASHTABLE、bucket、capacity 等内部编码，也不混入 source/target 不同的 ownership epoch；proof 另行绑定 slot、E/E+1、base identity 和 F。

source fence apply 后计算并在复制的 export state 中保存 final digest；target 在 `source_processed_through == F` 时计算相同 digest。摘要和 manifest checksum 是完整性门槛，但不是唯一所有权证明；写资格仍由 source committed fence、target committed activation 与 metadata CAS 的完整 proof chain 决定。

## 14. 故障处理矩阵

| 故障 | 必须行为 | 禁止行为 |
| --- | --- | --- |
| follower 宕机 | leader 在仍有多数时继续；副本恢复后追日志或装 snapshot | 降低 durable quorum 偷偷确认 |
| leader 宕机 | 多数派重新选举；客户端暂收 `TRYAGAIN`/重定向 | 两个 term 的 leader 同时确认写 |
| leader 落入少数分区 | CheckQuorum 后停止强一致读写 | 依靠本地“仍是 leader”继续返回成功 |
| 分片失去多数 | 只让该分片不可用 | 自动移除失联 voter 形成新 quorum |
| 控制面失去多数 | 稳定分片按最后拓扑继续；已 fence 迁移最多推进到安全 ASK-only 并高优报警 | 发起新变化、提交正式 owner、超时解除 fence |
| 磁盘满/`fsync` 失败 | 副本停止 ack；leader 退位或拒写；报警 | 只写 stderr 后继续确认 |
| WAL 中间损坏 | 隔离并人工/从健康副本修复 | 截断中间数据后当作正常节点加入 |
| WAL 尾部 torn write | 校验后仅截断最后不完整 record | 忽略 checksum 继续重放 |
| snapshot 安装中断 | 保留旧 active snapshot，从 chunk offset 重试 | 暴露半安装状态 |
| 节点重启 | 完整恢复后再服务，复用永久 node ID | 空库启动或生成新身份 |
| 客户端响应丢失 | 告知结果未知，允许业务幂等重试 | 把超时解释为未提交 |
| 迁移中 source 故障 | 新 leader 从 Raft 状态和 WAL pin 继续 | 绕过 fence 或另起同 epoch 迁移 |
| 迁移中 target 故障 | target group 选主并从 staging 状态继续 | source 已 fence 后随意回滚 |
| 时钟跳变 | Raft 安全计时不受影响；TTL 另行处理 | 用 wall clock 决定选举或 ReadIndex 安全性 |

## 15. 并发、网络与资源治理

### 15.1 并发模型

- Client Reactor 负责 socket、RESP 和连接顺序，不持有可变数据库。
- 每个 Raft group 是 actor；Raft 状态与 `ShardStore` 只在其串行上下文修改。
- 不同 group 可在固定调度线程池并行。
- WAL I/O 可由专用线程批处理，但完成回调必须回到对应 group actor。
- snapshot worker 只读取不可变 snapshot/COW 旧版本。
- topology 以 `shared_ptr<const TopologySnapshot>` 发布，C++17 使用 `atomic_load/atomic_store` 的 `shared_ptr` 自由函数或短临界区 mutex，路由线程只读。
- 内部 RPC 与客户端 I/O 分离配额，避免大响应或慢客户端饿死心跳。

这一阶段不需要为每个对象添加 mutex，也不应先引入固定 shard 锁。本文中的“分布式 shard”是 Raft 状态机边界，与《读写并发、写时复制与锁设计》中进程内并行访问 map 的 lock shard 不是同一概念。优先使用 actor ownership；只有一个 group 内部确有并行读收益且经过 profile 证明，才评估锁。

### 15.2 网络改造

在集群化前落实《网络性能分析与升级建议》的基础项：

- accept 循环到 `EAGAIN`，使用 `accept4` 设置 nonblocking/CLOEXEC；
- 读写缓冲改为 offset/分块队列，避免反复 `erase(0, n)`；
- 限制单轮读取字节、解析命令数、执行数和写出字节，使用 ready queue 保证公平；
- 输入、单 bulk、argc、pipeline、输出和连接数均设软/硬上限；
- 输出高水位暂停读，低水位恢复读；
- interest mask 变化时才执行 `EPOLL_CTL_MOD`；
- client、Raft、snapshot、migration 和 admin 使用独立监听端口与资源池；
- RPC frame 在分配内存前校验 magic、version、body length、checksum 和身份。

### 15.3 背压传播

```text
磁盘 fsync 变慢
  -> WAL pending bytes 达高水位
  -> group 暂停接收新 proposal
  -> router 返回 BUSY/TRYAGAIN 或暂停连接读取
  -> 保留 Raft heartbeat/vote 资源
```

迁移、snapshot 和 follower catch-up 是后台大流量，必须可限速并让位于心跳和前台 committed log。不能靠无限队列“吸收”过载。

## 16. 配置、身份与安全

### 16.1 配置模型

当前固定端口和工作目录需要替换为显式配置。示意配置：

```yaml
# 可选只读断言；首次 bootstrap/join 后以 data_dir/identity 为准
expected_cluster_id: 8d9b...
expected_node_id: 2f31...
roles: [data]
client_listen: 0.0.0.0:6379
client_advertise: redis-a.example:6379
raft_listen: 10.0.0.12:16379
admin_listen: 127.0.0.1:17379
data_dir: /var/lib/mini-redis
metadata_seeds: [10.0.0.1:26379, 10.0.0.2:26379]
read_consistency: linearizable
wal_fsync: strict
```

`cluster_id` 和 `node_id` 不是普通可编辑配置：bootstrap/join 只在空目录中一次性创建并原子写入 `data_dir/identity`，之后以磁盘 identity 为准。配置中的 `expected_*` 仅用于启动校验，不会创建或覆盖身份；复制配置若导致 node ID 冲突必须 fail-closed。配置加载后生成不可变 `NodeConfig` 并校验监听/广播地址、目录、磁盘 identity、端口冲突、证书身份、资源上限和 feature version。动态配置只能通过控制面白名单项更新，不能任意替换共识安全参数。

### 16.2 安全边界

- Raft、snapshot、migration 和控制面 RPC 不与客户端 RESP 共用端口。
- 非本机实验部署必须使用双向 TLS，证书身份映射到 cluster/node ID。
- Admin API 单独认证授权，成员变化、迁移、bootstrap 和灾难恢复进入审计日志。
- advertised address 只作为网络目标，不能拼接为文件路径或 shell 参数。
- snapshot/WAL/RPC 长度有上限，防止整数溢出、压缩炸弹和磁盘耗尽。
- 客户端认证和 ACL 在生产化阶段补齐；完成前服务不得直接暴露到不可信网络。
- secret 不写入普通日志、metrics label 或 Raft payload。

阶段 1 可以在同机 loopback 上先验证 Raft；首次跨主机运行前必须实现 mTLS 与 node identity 校验。进入共享环境或宣称生产可用之前，mTLS、Admin 授权与审计是强制发布门禁，不是可选优化。

## 17. 可观测性

至少提供以下低基数指标：

- 请求：按 command/group/result 的吞吐、p50/p95/p99、重定向和超时；
- Reactor：event-loop stall、ready queue、读写缓冲、连接数、背压次数；
- Raft：role、term、commit/applied index、leader changes、proposal latency、ReadIndex latency；
- 复制：每 follower match index、lag bytes、snapshot install progress；
- WAL：append bytes、batch size、fsync latency/error、segment 数和磁盘水位；
- snapshot：创建耗时、pause、COW bytes、文件大小、失败次数；
- topology：当前 revision、slot 覆盖数、epoch mismatch、metadata watch lag；
- migration：phase、base/delta bytes、source/target index、WAL pin bytes；
- 存储：每 group key 数、对象内存、slot key 数和 state hash 检查结果。

结构化日志统一携带 `cluster_id`、`node_id`、`group_id`、`term`、`index`、`migration_id` 和 `request_id`（适用时），但不能把用户 key 作为默认 metrics label。

## 18. 对当前代码的落地映射

### 18.1 现有文件改造

| 当前位置 | 改造方向 |
| --- | --- |
| `src/main.cpp` | 删除全局 `g_db/g_cmd_handler`；解析配置；构造唯一 `DataNode`；安全信号通知与优雅退出 |
| `Server.hpp/.cpp` | 构造注入 `RequestService`；移除重复 `Database db_`；增加定时器、完成队列、会话和资源预算 |
| `Connection.hpp/.cpp` | offset/分块缓冲；`ClientSession`；请求/响应序号；ASKING/READONLY 状态；高低水位 |
| `Protocol.hpp/.cpp` | 协议上限；二进制安全；typed cluster errors；集群响应编码 |
| `Command.hpp/.cpp` | 注册表、`CommandSpec`、KeySpec；拆分静态校验、路由与状态机 apply |
| `Database.hpp/.cpp` | 逐步重构为 `ShardStore`；slot 索引、结构化结果、snapshot 接口；禁止外部可变对象逃逸 |
| `RedisObject` 与 `objects/*` | 增加受控 clone/逻辑序列化；保证 snapshot 隔离和规范 state hash |
| `Dict` | 为一致 snapshot 实现安全 clone 或逻辑导出；后台 rehash 仍服从 group actor |
| `Rdb.hpp/.cpp` | 抽出对象 codec；RDB 只作为 import/export；损坏默认 fail-closed |
| `CMakeLists.txt` | 拆分 storage/raft/cluster/rpc 库，加入 sanitizer、模拟器和多进程集成测试目标 |

### 18.2 建议新增模块

```text
include/mini_redis/
├── app/
│   ├── DataNode.hpp
│   └── NodeConfig.hpp
├── cluster/
│   ├── Slot.hpp
│   ├── Topology.hpp
│   ├── Router.hpp
│   ├── MetadataStateMachine.hpp
│   └── MigrationStateMachine.hpp
├── command/
│   ├── CommandRegistry.hpp
│   ├── CommandSpec.hpp
│   └── CanonicalCommand.hpp
├── consensus/
│   ├── RaftCore.hpp
│   ├── RaftGroup.hpp
│   ├── RaftLog.hpp
│   └── RaftTransport.hpp
├── storage/
│   ├── ShardStore.hpp
│   ├── Wal.hpp
│   ├── Snapshot.hpp
│   └── ObjectCodec.hpp
└── rpc/
    ├── Frame.hpp
    └── InternalServer.hpp
```

Raft core 应设计成纯状态机：输入 tick、RPC、proposal 和 durable completion，输出待发送消息、待持久化记录和 committed entries。这样才能用虚拟时间做确定性故障模拟，而不是把 socket、线程、磁盘直接写进共识算法。

## 19. 分阶段实施计划

### 阶段 0：单机内核重构

工作项：

- 消除全局 handler 和重复数据库，建立唯一状态所有者；
- 引入 `NodeConfig`、显式 data dir 和安全退出；
- 实现命令注册表、KeySpec、规范命令与结构化结果；
- 实现 CRC16 slot/hash tag 与 typed cluster error；
- 把 `Database` 包装/重构为单 group `ShardStore`，初始拥有全部 slot；
- 建立确定性 apply/replay 和规范 state hash；
- 完成网络缓冲、预算、背压与协议上限；
- RDB 损坏从空启动改为 fail-closed；
- 建立一致 snapshot 边界，先深复制，随后可切 COW。

验收门槛：

- 现有 8 项 CTest 全部通过；
- slot/hash tag 与官方向量一致，包括二进制 key 和括号边界；
- 同一规范命令流重放到两个独立 store，逐 index state hash 一致；
- 任何业务写入都无法绕过 `ShardStore::apply`；
- 真实 Linux TCP/epoll 集成测试覆盖慢客户端、深 pipeline 和优雅退出。

### 阶段 1：单 Raft Group、三副本

工作项：

- 实现纯 Raft core、PreVote、CheckQuorum、ReadIndex；
- 实现版本化内部 RPC、严格 WAL、hard state 和 snapshot 安装；
- 实现静态 bootstrap manifest 和三节点本地开发启动器；首次跨主机运行前完成内部 mTLS 与 node identity 校验；
- 所有 16384 slots 暂由一个 group 拥有；
- 写命令全部经 Raft，所有访问 key 的默认强一致读经 ReadIndex；`PING`、`ASKING` 等无 key 会话命令仍本地执行；
- 新 leader 先提交当前 term 的 no-op/barrier，未完成前拒绝或等待强一致读；
- 实现恢复、日志冲突截断、snapshot 压缩和 leader transfer。

验收门槛：

- leader 返回成功后立即 `SIGKILL`，新 leader 仍能读到该写；
- 丢包、重复、乱序、分区、节点暂停下满足 election safety、log matching 和 state machine safety；
- 少数分区不能确认读写；
- torn WAL 尾可安全恢复，中间损坏拒绝启动；
- 随机读写历史通过线性一致性检查。

### 阶段 2：静态多分片与控制面

工作项：

- 复用 Raft engine 实现 Metadata group；
- 一个节点承载多个 `ShardReplica`；
- 静态分配 slot 到多个 data group；
- 实现 topology watch、revision/epoch 和持久缓存；
- 实现 `MOVED`、`CROSSSLOT` 和 RESP2 `CLUSTER SLOTS`；`CLUSTER SHARDS` 留到格式兼容测试完备后；
- 保证同连接异步响应有序。

验收门槛：

- cluster-aware 客户端可以发现拓扑并在 `MOVED` 后刷新；
- 16384 slots 恰好覆盖一次，无 owner/重复 owner 时集群不能标记健康；
- 同 slot 多 key 原子，跨 slot 稳定返回 `CROSSSLOT`；
- 一个分片失去 quorum 不影响其他分片；
- 控制面短暂失联不影响稳定分片的已有读写。

### 阶段 3：成员变更与高可用运维

工作项：

- learner、InstallSnapshot catch-up、joint consensus 和 leader transfer；
- placement/failure-domain 校验；
- 节点 drain、替换和滚动重启；
- 磁盘错误、lag、quorum、leader 抖动告警；
- 可选 stale follower read 及其显式协议/指标。

验收门槛：

- 任一 voter 宕机后，在测试默认 5 秒内恢复服务且无已确认写丢失；生产 RTO 由 `election_timeout`、重试和部署网络共同设定；
- joint configuration 的每个故障注入点均不出现两个可提交多数派；
- learner 的 `match_index` 落后 leader 超过 1024 entries 或 64 MiB（测试默认，可配置）时无法被提升；
- 控制面重复请求不会重复添加、移除成员。

### 阶段 4：在线 Slot 迁移

工作项：

- 迁移状态机、slot snapshot、增量流和 WAL pin；
- target staging、source fence、activation proof 与 metadata CAS；
- `ASK`/`ASKING`、迁移限速、磁盘配额和清理保留期；
- 迁移中 source/target leader 切换与进程重启恢复。

验收门槛：

- 对五种对象持续读写时迁移 slot，最终规范 state hash 与参考模型一致；
- 每个迁移阶段注入宕机、丢包、重复 chunk 和控制面失联后均可恢复；
- 任何历史中不存在源、目标同时确认该 slot 写入；
- metadata commit 后旧 owner 永远只重定向；
- fence 前迁移失败可以自动 abort 并释放 WAL/staging；
- fence 后控制面或数据 group 长期失去 quorum 时，系统进入有界配额和告警状态，不继续无上限扩大 WAL/staging；释放资源必须通过恢复 quorum 后 roll-forward cleanup，或执行显式灾难恢复流程。

### 阶段 5：生产化补强与后续语义

工作项：

- 强化证书轮换与吊销；补齐客户端认证/ACL、细粒度管理授权和审计查询（内部 mTLS 已在首次跨主机 Raft 前置）；
- 完整 metrics、trace、容量水位和自动 backpressure；
- 格式兼容矩阵、feature negotiation 和滚动升级；
- 备份、恢复演练、显式灾难恢复手册；
- TTL、`SCAN`、增强幂等 session；
- 最后再评估事务、跨 shard 查询或代理层。

## 20. 测试体系

### 20.1 确定性单元测试

- CRC16、hash tag、空括号、缺失右括号、二进制 key；
- CommandSpec、arity、KeySpec、同/跨 slot；
- CanonicalCommand 编解码、未知版本、长度溢出；
- 相同日志前缀在不同副本产生相同 state hash；恢复时同一 index 不会在已恢复状态上二次作用；
- 重复 `MigrationDelta` 按 migration ID/source index 幂等去重；不要求 `LPUSH/LPOP` 等用户命令本身幂等；
- WAL checksum、segment rollover、torn tail；
- snapshot canonical ordering、CRC 和原子激活；
- ownership epoch、旧 proposal no-op 和 ASKING 一次性状态。

### 20.2 Raft 确定性模拟器

使用虚拟时间和内存持久层注入：

- 消息丢失、重复、乱序和延迟；
- 任意双向/单向网络分区；
- 节点 crash/restart、长暂停和磁盘完成乱序；
- leader 连续切换、旧 term 消息和日志冲突；
- snapshot 与 AppendEntries 交错；
- joint consensus 每一步故障。

自动检查 election safety、leader completeness、log matching、state machine safety 和单调 commit/applied index。

### 20.3 多进程集成与故障测试

在 Linux 启动真实 3/5 节点进程，覆盖：

- TCP/epoll、redis-cli 与 cluster-aware client；
- 随机 `SIGKILL`、重启、磁盘满、只读文件系统、慢 fsync；
- 控制面 quorum 丢失、data group 独立故障；
- snapshot 传输中断、WAL 尾损坏和 identity 错配；
- 慢读/慢写、大请求、深 pipeline 和连接风暴；
- slot 迁移的每阶段 fault injection。

### 20.4 历史正确性检查

并发执行 GET/SET、List、Hash、Set、ZSet 和同 slot 多 key 命令，记录 invocation/completion、client、group、term、index 和结果，再由线性一致性检查器与参考模型验证。

客户端超时操作必须按“可能已发生”建模，不能直接从历史删除；迁移测试还要检查每个 epoch 最多一个确认写 owner。

### 20.5 发布门禁

发布分布式版本前至少满足：

- 当前单机命令与 RDB 回归测试全部保留；
- 少数网络分区无法确认写；
- 已确认写在任意单节点故障后不丢失；
- ReadIndex 历史通过线性一致性检查；
- WAL/snapshot 损坏不会静默空启动；
- Redis Cluster-aware 客户端正确处理 `MOVED` 和 `ASK`；
- 任意迁移阶段故障后恢复为唯一 owner；
- 未授权调用方无法访问内部 Raft、迁移或管理 API；
- 超大 RESP、慢客户端、catch-up 和迁移均有可验证的内存/磁盘上限。

### 20.6 可复现执行入口与默认测试参数

以下是后续实现必须提供的稳定入口；名称可在实现 PR 中调整一次，但 CI 和文档必须同步，不能只依赖人工步骤：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DMINI_REDIS_SANITIZERS=address,undefined
cmake --build build -j
ctest --test-dir build --label-regex 'unit|single-node' --output-on-failure

# 确定性 Raft 模拟器；失败时打印并保存 seed 与事件 trace
./build/bin/raft_sim --seed 1 --cases 10000 --max-virtual-ms 600000

# 在隔离的 Linux CI namespace 启动静态三节点并执行故障矩阵
./tests/cluster/run_cluster.sh --nodes 3 --replicas 3 --tls on --work-dir <temp-dir>
ctest --test-dir build --label-regex 'cluster|fault|linearizability' \
  --output-on-failure --timeout 300
```

脚本只能清理自己用 `mktemp -d` 创建并校验过的目录。每个失败 case 保留 bootstrap manifest、随机 seed、节点配置、结构化日志、RPC trace、WAL/snapshot manifest 和客户端历史；成功 case 可清理临时目录。Raft 模拟 CI 默认固定 seed 集合加 10000 个派生 case，每例虚拟时间最多 10 分钟；nightly 使用至少 100000 个 case。线性一致性 checker 读取 JSONL invocation/completion 历史，无法证明、超时或发现反例都以非零退出。

默认测试上限（均应成为配置项并有边界测试）：

| 项目 | 测试默认值 | 超限行为 |
| --- | ---: | --- |
| election timeout | 300–600 ms 随机 | 5 秒内未恢复 leader 则失败 |
| learner 晋升 lag | 1024 entries 且 64 MiB | 任一维超限禁止晋升 |
| 单 RESP frame / bulk | 64 MiB / 32 MiB | 协议错误并关闭连接 |
| 单连接输入 / 输出硬上限 | 64 MiB / 128 MiB | 暂停读后仍超限则关闭 |
| 单连接 pending requests | 1024 | 暂停 `EPOLLIN` |
| migration staging | 目标数据估算的 1.2 倍，且受磁盘水位约束 | PREPARE/COPY 拒绝或安全暂停 |
| migration WAL pin | 10 GiB 或 30 分钟 | fence 前自动进入 replicated abort；fence 后报警并限流 roll-forward |
| snapshot/RPC chunk | 4 MiB | 发送端拆分，接收端拒绝超限 frame |

这些值是 CI 和小型测试集群的初值，不是生产容量建议；压测后可修改默认值，但每次修改必须保留明确的超限语义。

阶段 feature 默认关闭并按 `single_node -> raft_v1 -> metadata_v1 -> migration_v1` 单向启用。WAL、snapshot 和 metadata 的 writer 只有在全部相关 voter 宣告可读该版本后才升级；升级后旧二进制若不能读取磁盘上的 `min_reader_version`，必须拒绝启动。本文不承诺通用原地 downgrade。已进入 `SOURCE_FENCED` 的迁移在任何代码回退前都必须完成 roll-forward，不能通过降级二进制绕过 fence。

### 20.7 客户端兼容矩阵

首个兼容门槛固定验证 `redis-cli` 与至少一个具体版本的 cluster-aware 客户端（建议 redis-py 7.x；实现时将精确 patch version 固定在 CI lockfile）。测试覆盖首次连接、`CLUSTER SLOTS` 发现、连接池刷新、`MOVED`、`ASKING + command`、pipeline、IPv4、IPv6 和 hostname advertise。

若所选客户端连接初始化会发送 `COMMAND`、`CLIENT SETINFO`、`CLIENT ID`、`READONLY` 等命令，服务端必须实现其所需最小兼容子集或在兼容矩阵中明确排除对应模式。只有矩阵中的固定客户端版本通过真实多进程测试后，文档才宣称该客户端受支持；其他客户端仅称“协议设计兼容”，不作可用性承诺。

## 21. TTL 的后续设计约束

TTL 当前尚未实现，不应与第一版 Raft 一起开发。未来加入时必须保证副本不会依据各自墙上时间独立删除 key：

1. leader 将过期参数转换为日志中的绝对 deadline，并随 key version 一起复制；
2. 所有副本 apply 相同 deadline，但只有 leader 调度过期；
3. deadline 到达后，leader proposal `ExpireIfVersion(key, version, deadline)`；
4. 删除只在该 entry committed/apply 后发生；
5. 强一致读若发现 key 到期，先推动对应过期 entry，再返回不存在；
6. snapshot 保存 deadline/version；leader 切换后重建定时索引；
7. follower stale read 只反映已 apply 的过期记录，不额外承诺实时 TTL。

墙上时钟变化可能影响“何时”过期，但不能造成各副本状态分叉。Raft 选举、心跳和 ReadIndex 仍只使用 monotonic clock。

## 22. 主要风险与取舍

| 风险/代价 | 影响 | 缓解 |
| --- | --- | --- |
| Raft 写放大与 fsync 延迟 | 单写延迟高于当前内存写 | group commit、批量 append、多个 shard 并行，不能降低安全门槛冒充强持久 |
| 热 slot/热 key | 单 group actor 成为瓶颈 | hash tag 规范、slot 迁移、容量指标；热点单 key 天然无法水平拆分 |
| 一个节点承载过多 group | timer、mailbox、WAL fd 和调度成本上升 | group 数量基准、固定调度池、合并 slot，而非每 slot 一组 |
| COW snapshot 内存峰值 | 写热点期间复制大量对象 | snapshot 配额、单节点并发数限制、pause/defer 策略 |
| 迁移 WAL pin | 慢迁移阻止日志回收 | 限速、超时、容量预检、可观测 pin bytes 和安全取消点 |
| Cluster 兼容不完整 | 某些客户端命令行为不同 | 明确支持矩阵，先实现 `CLUSTER SLOTS` 与重定向核心路径 |
| 跨 slot 能力受限 | 应用需设计 hash tag | 文档、客户端辅助；不以首版 2PC 换取高复杂度 |
| 控制面成为运维复杂点 | 需要额外 quorum | 逻辑隔离、稳定分片不依赖热路径、同一 Raft engine 复用 |
| 格式升级不兼容 | rolling upgrade 可能使旧节点失效 | codec version、feature negotiation、先升级 readers 后启用 writers |

## 23. 完成定义

只有同时满足以下条件，项目才可以称为“支持分布式”：

1. key 能按官方兼容 slot 算法稳定路由到多个逻辑 shard；
2. 每个 shard 至少可用 3 副本 Raft 运行，自动选主且少数派拒写；
3. 所有写都经 durable committed log，默认读经过 ReadIndex；
4. 节点可从 snapshot + WAL 严格恢复，损坏不会空库启动；
5. 20.7 节固定版本的 cluster-aware 客户端能通过 `CLUSTER SLOTS`、`MOVED` 和 `ASK` 正确访问；
6. 同 slot 多 key 写原子 apply、读共享一个 ReadIndex barrier，跨 slot 明确返回 `CROSSSLOT`；
7. slot 可以在持续流量和故障注入下迁移，始终保持唯一写 owner；
8. 成员变化使用 learner + joint consensus；
9. 客户端、内部 RPC、WAL、snapshot 和迁移都有资源上限与可观测性；
10. 确定性模拟、多进程故障测试和线性一致性历史检查全部通过。

在此之前，更准确的表述应是“正在演进为分布式 Redis 的单机内核”或“具备某个分布式阶段能力”，而不是生产级 Redis Cluster。
