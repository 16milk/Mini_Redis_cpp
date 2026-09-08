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
    std::string cluster_config_path;
    std::string cluster_self_id;
};

void printUsage(const char* program) {
    std::cerr << "usage: " << program << " [--port <port>]"
              << " [--cluster-config <file>] [--cluster-self <node-id>]" << std::endl;
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool needs_value = argument == "--port" ||
                                 argument == "--cluster-config" ||
                                 argument == "--cluster-self";
        if (needs_value && index + 1 >= argc) {
            std::cerr << "[FATAL] missing value for " << argument << std::endl;
            return false;
        }

        if (argument == "--port") {
            options.port = std::atoi(argv[++index]);
            if (options.port <= 0 || options.port > 65535) {
                std::cerr << "[FATAL] invalid port" << std::endl;
                return false;
            }
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

        Server server(database, options.port, &shutdown_flag, router.get());
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
