#include "mini_redis/net/Server.hpp"

#include "mini_redis/cluster/Router.hpp"
#include "mini_redis/net/utils.hpp"
#include "mini_redis/runtime/RequestFrontend.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>

namespace {

unsigned defaultParallelism() {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        return 1;
    }
    return hardware < 4 ? hardware : 4;
}

}  // namespace

unsigned Server::chooseParallelism(unsigned requested) {
    if (requested == 0) {
        return defaultParallelism();
    }
    return requested;
}

Server::Server(int port)
    : owned_db_(std::make_unique<Database>()),
      db_(*owned_db_),
      command_handler_(db_),
      options_{},
      shutdown_flag_(nullptr),
      router_(nullptr),
      scheduler_(chooseParallelism(0)) {
    options_.port = port;
    options_.client_reactor_count = chooseParallelism(0);
    options_.scheduler_workers = scheduler_.workerCount();
    create_group_actors();
    create_reactors();
}

Server::Server(Database& database, int port, volatile std::sig_atomic_t* shutdown_flag,
               cluster::ClusterRouter* router)
    : Server(database, ServerOptions{}, shutdown_flag, router) {
    options_.port = port;
}

Server::Server(Database& database, ServerOptions options,
               volatile std::sig_atomic_t* shutdown_flag,
               cluster::ClusterRouter* router)
    : owned_db_(nullptr),
      db_(database),
      command_handler_(database, router),
      options_(std::move(options)),
      shutdown_flag_(shutdown_flag),
      router_(router),
      scheduler_(chooseParallelism(options.scheduler_workers)) {
    options_.client_reactor_count = chooseParallelism(options_.client_reactor_count);
    options_.scheduler_workers = scheduler_.workerCount();
    create_group_actors();
    create_reactors();
}

Server::~Server() {
    for (auto& reactor : reactors_) {
        if (reactor) {
            reactor->requestStop();
        }
    }
    for (auto& thread : reactor_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    scheduler_.stop();
    auto close_fd = [](int& fd) {
        if (fd != -1) {
            close(fd);
            fd = -1;
        }
    };
    close_fd(client_listen_fd_);
    close_fd(raft_listen_fd_);
    close_fd(snapshot_listen_fd_);
    close_fd(migration_listen_fd_);
    close_fd(admin_listen_fd_);
}

void Server::create_group_actors() {
    if (router_ == nullptr) {
        scheduler_.addGroup(runtime::kStandaloneGroupId, db_);
        return;
    }
    const auto topology = router_->topology();
    std::vector<cluster::ShardId> local_shards;
    if (topology) {
        local_shards = topology->shardsHostedBy(router_->selfId());
    }
    if (local_shards.empty()) {
        scheduler_.addGroup(runtime::kStandaloneGroupId, db_, router_);
        return;
    }
    bool first = true;
    for (const cluster::ShardId shard : local_shards) {
        const auto group_id = static_cast<consensus::GroupId>(shard);
        if (first) {
            scheduler_.addGroup(group_id, db_, router_);
            first = false;
            continue;
        }
        extra_group_dbs_.push_back(std::make_unique<Database>(true));
        scheduler_.addGroup(group_id, *extra_group_dbs_.back(), router_);
    }
}

void Server::create_reactors() {
    const unsigned count = options_.client_reactor_count == 0 ? 1 : options_.client_reactor_count;
    reactors_.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        reactors_.push_back(std::make_unique<Reactor>(
            static_cast<int>(index), scheduler_, command_handler_, router_, &pools_,
            shutdown_flag_));
    }

    auto assigner = [this](runtime::TrafficClass traffic) -> Reactor* {
        if (reactors_.empty()) {
            return nullptr;
        }
        if (runtime::isControlTraffic(traffic)) {
            return reactors_.front().get();
        }
        if (runtime::isBulkTraffic(traffic)) {
            return reactors_.back().get();
        }
        const unsigned index =
            next_client_reactor_.fetch_add(1, std::memory_order_relaxed) %
            static_cast<unsigned>(reactors_.size());
        return reactors_[index].get();
    };
    for (auto& reactor : reactors_) {
        reactor->setAssigner(assigner);
    }
}

int Server::create_listen_socket(int port, const char* label) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        handle_error("socket");
    }
    set_nonblocking(fd);
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        handle_error("setsockopt SO_REUSEADDR");
    }
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        handle_error("bind");
    }
    if (listen(fd, 128) == -1) {
        handle_error("listen");
    }
    std::cout << "[INFO] " << label << " listening on port " << port << std::endl;
    return fd;
}

void Server::setup_listeners() {
    client_listen_fd_ = create_listen_socket(options_.port, "client");
    reactors_.front()->addListenSocket(client_listen_fd_, runtime::TrafficClass::kClient);
    if (options_.raft_port > 0) {
        raft_listen_fd_ = create_listen_socket(options_.raft_port, "raft");
        reactors_.front()->addListenSocket(raft_listen_fd_, runtime::TrafficClass::kRaft);
    }
    if (options_.admin_port > 0) {
        admin_listen_fd_ = create_listen_socket(options_.admin_port, "admin");
        reactors_.front()->addListenSocket(admin_listen_fd_, runtime::TrafficClass::kAdmin);
    }
    if (options_.snapshot_port > 0) {
        snapshot_listen_fd_ = create_listen_socket(options_.snapshot_port, "snapshot");
        reactors_.front()->addListenSocket(snapshot_listen_fd_,
                                           runtime::TrafficClass::kSnapshot);
    }
    if (options_.migration_port > 0) {
        migration_listen_fd_ = create_listen_socket(options_.migration_port, "migration");
        reactors_.front()->addListenSocket(migration_listen_fd_,
                                           runtime::TrafficClass::kMigration);
    }
}

void Server::run() {
    setup_listeners();
    scheduler_.start();

    std::cout << "[INFO] Multi-reactor runtime: reactors=" << reactors_.size()
              << " group_workers=" << scheduler_.workerCount()
              << " groups=" << scheduler_.groupCount() << std::endl;

    for (std::size_t index = 1; index < reactors_.size(); ++index) {
        Reactor* reactor = reactors_[index].get();
        reactor_threads_.emplace_back([reactor] { reactor->run(); });
    }
    reactors_.front()->run();

    for (auto& reactor : reactors_) {
        reactor->requestStop();
    }
    for (auto& thread : reactor_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    reactor_threads_.clear();
    scheduler_.stop();
    std::cout << "[INFO] Shutdown requested; leaving event loop." << std::endl;
}
