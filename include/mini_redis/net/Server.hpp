#pragma once

#include "mini_redis/command/Command.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/net/Connection.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

class Server {
public:
    // Compatibility constructor: owns one database and uses it for commands and maintenance.
    explicit Server(int port = 6380);
    Server(Database& database, int port = 6380,
           volatile std::sig_atomic_t* shutdown_flag = nullptr);
    ~Server();
    void run();  // 主事件循环

private:
    struct InputProcessResult {
        std::size_t commands_processed = 0;
        bool needs_more_processing = false;
        bool protocol_error = false;
    };

    struct PendingClientWork {
        std::uint32_t events = 0;
        bool continue_read = false;
    };

    void setup_listen_socket();
    void accept_client();
    void close_client(int fd);
    bool update_client_events(int fd, const Connection& connection);
    InputProcessResult process_client_input(
        Connection& connection, std::size_t max_commands,
        std::chrono::microseconds max_runtime);
    void enqueue_client(int fd, std::uint32_t events = 0, bool continue_read = false);
    void process_ready_clients();
    int epoll_timeout_ms(bool expire_work_remains) const;
    bool shutdown_requested() const;

    std::unique_ptr<Database> owned_db_;
    Database& db_;
    CommandHandler command_handler_;
    int port_;
    int listen_fd_;
    int epoll_fd_;
    volatile std::sig_atomic_t* shutdown_flag_;
    
    // 管理所有客户端连接：fd -> Connection
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::deque<int> ready_clients_;
    std::unordered_set<int> queued_clients_;
    std::unordered_map<int, PendingClientWork> pending_client_work_;
    std::unordered_set<int> close_after_write_;
};
