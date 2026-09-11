#include "mini_redis/cluster/ClusterConfig.hpp"
#include "mini_redis/cluster/Router.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/net/Server.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

volatile std::sig_atomic_t shutdown_flag = 0;

void signal_handler(int) {
    // Assignment to sig_atomic_t is the only operation performed in signal
    // context. Persistence and logging happen after the event loop returns.
    shutdown_flag = 1;
}

struct Options {
    int port = 6379;
    int raft_port = 0;
    int snapshot_port = 0;
    int migration_port = 0;
    int admin_port = 0;
    unsigned reactors = 0;
    unsigned workers = 0;
    std::string cluster_config_path;
    std::string cluster_self_id;
};

void printUsage(const char* program) {
    std::cerr << "usage: " << program << " [--port <port>]"
              << " [--raft-port <port>] [--snapshot-port <port>]"
              << " [--migration-port <port>] [--admin-port <port>]"
              << " [--reactors <n>] [--workers <n>]"
              << " [--cluster-config <file>] [--cluster-self <node-id>]" << std::endl;
}

bool parsePort(const char* text, int& out) {
    out = std::atoi(text);
    return out > 0 && out <= 65535;
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool needs_value = argument == "--port" ||
                                 argument == "--raft-port" ||
                                 argument == "--snapshot-port" ||
                                 argument == "--migration-port" ||
                                 argument == "--admin-port" ||
                                 argument == "--reactors" ||
                                 argument == "--workers" ||
                                 argument == "--cluster-config" ||
                                 argument == "--cluster-self";
        if (needs_value && index + 1 >= argc) {
            std::cerr << "[FATAL] missing value for " << argument << std::endl;
            return false;
        }

        if (argument == "--port") {
            if (!parsePort(argv[++index], options.port)) {
                std::cerr << "[FATAL] invalid port" << std::endl;
                return false;
            }
        } else if (argument == "--raft-port") {
            if (!parsePort(argv[++index], options.raft_port)) {
                std::cerr << "[FATAL] invalid raft port" << std::endl;
                return false;
            }
        } else if (argument == "--snapshot-port") {
            if (!parsePort(argv[++index], options.snapshot_port)) {
                std::cerr << "[FATAL] invalid snapshot port" << std::endl;
                return false;
            }
        } else if (argument == "--migration-port") {
            if (!parsePort(argv[++index], options.migration_port)) {
                std::cerr << "[FATAL] invalid migration port" << std::endl;
                return false;
            }
        } else if (argument == "--admin-port") {
            if (!parsePort(argv[++index], options.admin_port)) {
                std::cerr << "[FATAL] invalid admin port" << std::endl;
                return false;
            }
        } else if (argument == "--reactors") {
            const int value = std::atoi(argv[++index]);
            if (value <= 0 || value > 64) {
                std::cerr << "[FATAL] invalid reactor count" << std::endl;
                return false;
            }
            options.reactors = static_cast<unsigned>(value);
        } else if (argument == "--workers") {
            const int value = std::atoi(argv[++index]);
            if (value <= 0 || value > 64) {
                std::cerr << "[FATAL] invalid worker count" << std::endl;
                return false;
            }
            options.workers = static_cast<unsigned>(value);
        } else if (argument == "--cluster-config") {
            options.cluster_config_path = argv[++index];
        } else if (argument == "--cluster-self") {
            options.cluster_self_id = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            std::cerr << "[FATAL] unknown argument " << argument << std::endl;
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        Database database;

        // 集群模式下路由决策来自不可变拓扑快照。完成形态里快照由已提交的
        // metadata Raft group 推送，这里先从配置文件构造同一种快照。
        std::unique_ptr<cluster::ClusterRouter> router;
        if (!options.cluster_config_path.empty()) {
            const cluster::ClusterConfig config = cluster::loadClusterConfigFile(
                options.cluster_config_path, nowMs(), options.cluster_self_id);
            router = std::make_unique<cluster::ClusterRouter>(config.self_id,
                                                              config.topology);
            std::cout << "[INFO] cluster mode: node=" << config.self_id
                      << " raft_groups=" << config.topology->raftGroupCount()
                      << " slots_assigned=" << config.topology->assignedSlotCount()
                      << "/" << cluster::kSlotCount << std::endl;
        }

        ServerOptions server_options;
        server_options.port = options.port;
        server_options.raft_port = options.raft_port;
        server_options.snapshot_port = options.snapshot_port;
        server_options.migration_port = options.migration_port;
        server_options.admin_port = options.admin_port;
        server_options.client_reactor_count = options.reactors;
        server_options.scheduler_workers = options.workers;

        Server server(database, server_options, &shutdown_flag, router.get());
        server.run();

        if (!database.saveRdb()) {
            std::cerr << "[WARN] Failed to save database during shutdown" << std::endl;
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
