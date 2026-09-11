#pragma once

#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/net/Reactor.hpp"
#include "mini_redis/runtime/GroupScheduler.hpp"
#include "mini_redis/runtime/ResourcePool.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace cluster {
class ClusterRouter;
}  // namespace cluster

struct ServerOptions {
    int port = 6380;
    int raft_port = 0;
    int snapshot_port = 0;
    int migration_port = 0;
    int admin_port = 0;
    unsigned client_reactor_count = 0;  // 0 = min(4, hardware_concurrency)
    unsigned scheduler_workers = 0;     // 0 = min(4, hardware_concurrency)
};

class Server {
public:
    // Compatibility constructor: owns one database and uses it for commands and maintenance.
    explicit Server(int port = 6380);
    // A non-null router puts the node in cluster mode: commands are routed by
    // hash slot before execution. A null router keeps single-node behaviour.
    Server(Database& database, int port = 6380,
           volatile std::sig_atomic_t* shutdown_flag = nullptr,
           cluster::ClusterRouter* router = nullptr);
    Server(Database& database, ServerOptions options,
           volatile std::sig_atomic_t* shutdown_flag = nullptr,
           cluster::ClusterRouter* router = nullptr);
    ~Server();

    void run();  // 启动多 Reactor 与 Group Actor 调度器

    unsigned reactorCount() const { return static_cast<unsigned>(reactors_.size()); }
    unsigned workerCount() const { return scheduler_.workerCount(); }

private:
    static unsigned chooseParallelism(unsigned requested);
    int create_listen_socket(int port, const char* label);
    void setup_listeners();
    void create_group_actors();
    void create_reactors();

    std::unique_ptr<Database> owned_db_;
    Database& db_;
    CommandHandler command_handler_;
    ServerOptions options_;
    volatile std::sig_atomic_t* shutdown_flag_;
    cluster::ClusterRouter* router_;

    runtime::GroupScheduler scheduler_;
    runtime::ResourcePools pools_;
    std::vector<std::unique_ptr<Database>> extra_group_dbs_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::vector<std::thread> reactor_threads_;
    std::atomic<unsigned> next_client_reactor_{0};

    int client_listen_fd_ = -1;
    int raft_listen_fd_ = -1;
    int snapshot_listen_fd_ = -1;
    int migration_listen_fd_ = -1;
    int admin_listen_fd_ = -1;
};
