#pragma once

#include "mini_redis/command/Command.hpp"
#include "mini_redis/net/Connection.hpp"
#include "mini_redis/runtime/CompletionQueue.hpp"
#include "mini_redis/runtime/GroupScheduler.hpp"
#include "mini_redis/runtime/IoBudget.hpp"
#include "mini_redis/runtime/RequestFrontend.hpp"
#include "mini_redis/runtime/ResourcePool.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Reactor {
public:
    using AssignFn = std::function<Reactor*(runtime::TrafficClass)>;

    Reactor(int id, runtime::GroupScheduler& scheduler, CommandHandler& local_commands,
            cluster::ClusterRouter* router, runtime::ResourcePools* pools,
            volatile std::sig_atomic_t* shutdown_flag);

    ~Reactor();
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    int id() const { return id_; }
    runtime::CompletionQueue& completions() { return completions_; }

    void setAssigner(AssignFn assigner) { assigner_ = std::move(assigner); }
    void addListenSocket(int fd, runtime::TrafficClass traffic);
    void offerConnection(int fd, runtime::TrafficClass traffic);

    void run();
    void requestStop();
    bool shutdownRequested() const;

    std::size_t connectionCount() const { return connections_.size(); }

private:
    struct InputProcessResult {
        std::size_t commands_processed = 0;
        bool needs_more_processing = false;
        bool protocol_error = false;
        bool pause_reads = false;
    };

    struct PendingClientWork {
        std::uint32_t events = 0;
        bool continue_read = false;
    };

    struct IncomingSocket {
        int fd = -1;
        runtime::TrafficClass traffic = runtime::TrafficClass::kClient;
    };

    void accept_listen(int listen_fd, runtime::TrafficClass traffic);
    void adopt_client(int client_fd, runtime::TrafficClass traffic);
    void close_client(int fd);
    bool update_client_events(int fd, Connection& connection);
    void apply_completions();
    void adopt_inbound();
    void resume_downstream();
    void flush_pipeline(Connection& connection);
    InputProcessResult process_client_input(Connection& connection, std::size_t max_commands,
                                            std::chrono::microseconds max_runtime);
    void enqueue_client(int fd, std::uint32_t events = 0, bool continue_read = false);
    void process_ready_clients();
    int epoll_timeout_ms() const;
    std::deque<int>& readyQueueFor(runtime::TrafficClass traffic);

    int id_ = 0;
    runtime::GroupScheduler& scheduler_;
    runtime::RequestFrontend frontend_;
    runtime::ResourcePools* pools_ = nullptr;
    volatile std::sig_atomic_t* shutdown_flag_ = nullptr;
    AssignFn assigner_;

    int epoll_fd_ = -1;
    runtime::CompletionQueue completions_;

    std::unordered_map<int, runtime::TrafficClass> listen_fds_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::unordered_map<std::uint64_t, int> connection_ids_;

    std::deque<int> ready_control_;
    std::deque<int> ready_client_;
    std::deque<int> ready_bulk_;
    std::unordered_set<int> queued_clients_;
    std::unordered_map<int, PendingClientWork> pending_client_work_;
    std::unordered_set<int> close_after_write_;

    std::mutex inbound_mu_;
    std::vector<IncomingSocket> inbound_;
    std::chrono::steady_clock::time_point last_tick_{};
};
