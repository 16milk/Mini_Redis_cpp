#include "mini_redis/net/Server.hpp"

#include "mini_redis/net/Protocol.hpp"
#include "mini_redis/net/utils.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <vector>

namespace {

constexpr int kMaxEvents = 128;
constexpr int kBacklog = 128;
constexpr int kMaxMaintenanceIntervalMs = 100;
constexpr std::size_t kExpireKeysPerCycle = 64;
constexpr auto kExpireRuntime = std::chrono::milliseconds(1);

// These bounds make one always-readable client yield to the rest of the loop.
constexpr std::size_t kReadBytesPerTurn = 64 * 1024;
constexpr std::size_t kReadCallsPerTurn = 16;
constexpr std::size_t kInputHighWatermark = 1024 * 1024;
constexpr std::size_t kInputResumeWatermark = kInputHighWatermark / 2;
constexpr std::size_t kOutputHighWatermark = 4 * 1024 * 1024;
constexpr std::size_t kOutputResumeWatermark = kOutputHighWatermark / 2;
constexpr std::size_t kCommandsPerTurn = 64;
constexpr auto kCommandRuntimePerTurn = std::chrono::milliseconds(1);
constexpr std::size_t kWriteBytesPerTurn = 64 * 1024;
constexpr std::size_t kWriteCallsPerTurn = 16;

// A ready-queue round is also bounded so expiration maintenance runs regularly.
constexpr std::size_t kReadyClientsPerRound = 128;
constexpr auto kReadyRoundRuntime = std::chrono::milliseconds(5);

} // namespace

Server::Server(int port)
    : owned_db_(std::make_unique<Database>()),
      db_(*owned_db_),
      command_handler_(db_),
      port_(port),
      listen_fd_(-1),
      epoll_fd_(-1),
      shutdown_flag_(nullptr) {}

Server::Server(Database& database, int port,
               volatile std::sig_atomic_t* shutdown_flag)
    : owned_db_(nullptr),
      db_(database),
      command_handler_(database),
      port_(port),
      listen_fd_(-1),
      epoll_fd_(-1),
      shutdown_flag_(shutdown_flag) {}

Server::~Server() {
    connections_.clear();
    if (listen_fd_ != -1) close(listen_fd_);
    if (epoll_fd_ != -1) close(epoll_fd_);
}

void Server::setup_listen_socket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) handle_error("socket");

    set_nonblocking(listen_fd_);

    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
        handle_error("setsockopt SO_REUSEADDR");

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1)
        handle_error("bind");

    if (listen(listen_fd_, kBacklog) == -1)
        handle_error("listen");

    std::cout << "[INFO] Server listening on port " << port_ << std::endl;
}

void Server::accept_client() {
    // The listening socket uses level-triggered epoll, so one bounded accept per
    // event keeps this path fair; any backlog causes another readiness event.
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd = accept(
        listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            std::cerr << "[WARN] accept failed: " << std::strerror(errno) << std::endl;
        }
        return;
    }

    const int client_flags = fcntl(client_fd, F_GETFL, 0);
    if (client_flags == -1 ||
        fcntl(client_fd, F_SETFL, client_flags | O_NONBLOCK) == -1) {
        std::cerr << "[WARN] failed to make client nonblocking, fd=" << client_fd
                  << ": " << std::strerror(errno) << std::endl;
        close(client_fd);
        return;
    }

    auto connection = std::make_unique<Connection>(client_fd);
    struct epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = client_fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
        std::cerr << "[WARN] epoll_ctl add client failed, fd=" << client_fd
                  << ": " << std::strerror(errno) << std::endl;
        // The local connection remains its sole owner on failure.
        return;
    }

    connections_.emplace(client_fd, std::move(connection));
    std::cout << "[INFO] Client connected, fd=" << client_fd << std::endl;
}

void Server::close_client(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    connections_.erase(fd);
    queued_clients_.erase(fd);
    pending_client_work_.erase(fd);
    close_after_write_.erase(fd);
}

bool Server::update_client_events(int fd, const Connection& connection) {
    struct epoll_event event{};
    if (!connection.peerReadClosed() && close_after_write_.count(fd) == 0 &&
        connection.pendingInputBytes() < kInputHighWatermark &&
        connection.pendingWriteBytes() < kOutputResumeWatermark) {
        event.events |= EPOLLIN | EPOLLRDHUP;
    } else if (!connection.peerReadClosed()) {
        event.events |= EPOLLRDHUP;
    }
    if (connection.hasPendingWrite()) {
        event.events |= EPOLLOUT;
    }
    if (event.events == 0) {
        return false;
    }
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        std::cerr << "[WARN] epoll_ctl mod client failed, fd=" << fd
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

Server::InputProcessResult Server::process_client_input(
    Connection& connection, std::size_t max_commands,
    std::chrono::microseconds max_runtime) {
    InputProcessResult outcome;
    RespParser parser;
    const auto started = std::chrono::steady_clock::now();

    while (connection.hasPendingInput()) {
        if (outcome.commands_processed >= max_commands ||
            (outcome.commands_processed != 0 &&
             std::chrono::steady_clock::now() - started >= max_runtime)) {
            outcome.needs_more_processing = true;
            return outcome;
        }

        std::vector<std::string> arguments;
        std::size_t bytes_consumed = 0;
        const auto result =
            parser.parse(connection.getReadBuffer(), arguments, bytes_consumed);

        if (result == RespParser::INCOMPLETE) {
            if (connection.peerReadClosed()) {
                connection.sendResponse(RespParser::encodeError("protocol error"));
                connection.consumeInput(connection.getReadBuffer().size());
                outcome.protocol_error = true;
            }
            return outcome;
        }

        if (result == RespParser::ERROR) {
            connection.sendResponse(RespParser::encodeError("protocol error"));
            // A malformed request has no trustworthy frame boundary.
            connection.consumeInput(connection.getReadBuffer().size());
            outcome.protocol_error = true;
            return outcome;
        }

        connection.sendResponse(command_handler_.execute(arguments));
        connection.consumeInput(bytes_consumed);
        ++outcome.commands_processed;
        if (connection.pendingWriteBytes() >= kOutputHighWatermark) {
            outcome.needs_more_processing = connection.hasPendingInput();
            return outcome;
        }
    }

    return outcome;
}

void Server::enqueue_client(int fd, std::uint32_t events, bool continue_read) {
    if (connections_.find(fd) == connections_.end()) {
        return;
    }

    auto& work = pending_client_work_[fd];
    work.events |= events;
    work.continue_read = work.continue_read || continue_read;
    if (queued_clients_.insert(fd).second) {
        ready_clients_.push_back(fd);
    }
}

void Server::process_ready_clients() {
    const auto round_started = std::chrono::steady_clock::now();
    std::size_t clients_processed = 0;

    while (!ready_clients_.empty() && clients_processed < kReadyClientsPerRound &&
           std::chrono::steady_clock::now() - round_started < kReadyRoundRuntime) {
        const int fd = ready_clients_.front();
        ready_clients_.pop_front();
        queued_clients_.erase(fd);

        const auto work_it = pending_client_work_.find(fd);
        if (work_it == pending_client_work_.end()) {
            continue;
        }
        const PendingClientWork work = work_it->second;
        pending_client_work_.erase(work_it);

        auto connection_it = connections_.find(fd);
        if (connection_it == connections_.end()) {
            continue;
        }
        Connection& connection = *connection_it->second;
        ++clients_processed;

        if (work.events & EPOLLERR) {
            std::cout << "[INFO] Client error, fd=" << fd << std::endl;
            close_client(fd);
            continue;
        }

        bool continue_read = work.continue_read;
        const bool closing_after_write = close_after_write_.count(fd) != 0;
        const bool output_allows_commands =
            connection.pendingWriteBytes() < kOutputResumeWatermark;
        const bool commands_deferred_for_output =
            !closing_after_write && !output_allows_commands &&
            connection.hasPendingInput();
        const bool socket_readable =
            work.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP);
        const bool below_read_limit = connection.pendingInputBytes() < kInputHighWatermark;
        if (!closing_after_write && output_allows_commands &&
            !connection.peerReadClosed() &&
            (socket_readable || continue_read) &&
            below_read_limit) {
            const auto read_result = connection.readFromSocket(
                kReadBytesPerTurn, kReadCallsPerTurn);
            if (!read_result.ok) {
                std::cout << "[INFO] Client read failed, fd=" << fd << std::endl;
                close_client(fd);
                continue;
            }
            continue_read = read_result.budget_exhausted;
        } else {
            continue_read = false;
        }

        InputProcessResult input_result;
        if (!closing_after_write && output_allows_commands) {
            input_result = process_client_input(
                connection, kCommandsPerTurn,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    kCommandRuntimePerTurn));
        } else if (closing_after_write) {
            connection.discardInput();
        }
        if (input_result.protocol_error) {
            close_after_write_.insert(fd);
        }
        const bool must_close_after_write = close_after_write_.count(fd) != 0;

        if ((work.events & EPOLLOUT) || connection.hasPendingWrite()) {
            if (!connection.writeToSocket(kWriteBytesPerTurn, kWriteCallsPerTurn)) {
                std::cout << "[INFO] Failed to write to client, closing fd="
                          << fd << std::endl;
                close_client(fd);
                continue;
            }
        }
        const bool output_below_resume =
            connection.pendingWriteBytes() < kOutputResumeWatermark;

        if (connection.peerReadClosed() && !connection.hasPendingInput() &&
            !connection.hasPendingWrite()) {
            std::cout << "[INFO] Client disconnected, fd=" << fd << std::endl;
            close_client(fd);
            continue;
        }
        if (must_close_after_write && !connection.hasPendingWrite()) {
            close_client(fd);
            continue;
        }

        if (!update_client_events(fd, connection)) {
            close_client(fd);
            continue;
        }

        // Requeue only work known to remain in userspace or a read that stopped
        // at its budget. Incomplete buffered frames wait for the next EPOLLIN.
        if (connection.pendingInputBytes() >= kInputHighWatermark &&
            output_allows_commands &&
            !input_result.needs_more_processing) {
            std::cout << "[INFO] Client input buffer limit exceeded, fd="
                      << fd << std::endl;
            close_client(fd);
            continue;
        }

        const bool resume_socket_read =
            continue_read && connection.pendingInputBytes() < kInputResumeWatermark;
        const bool resume_buffered_commands =
            input_result.needs_more_processing &&
            output_below_resume;
        const bool output_unblocked_buffered_input =
            commands_deferred_for_output && output_below_resume &&
            connection.hasPendingInput();
        if (resume_socket_read || resume_buffered_commands ||
            output_unblocked_buffered_input) {
            enqueue_client(fd, 0, resume_socket_read);
        }
    }
}

int Server::epoll_timeout_ms(bool expire_work_remains) const {
    if (!ready_clients_.empty() || expire_work_remains) {
        return 0;
    }

    const auto next_expire = db_.nextExpireAt();
    if (!next_expire) {
        return kMaxMaintenanceIntervalMs;
    }

    const UnixMillis now_ms = db_.nowMs();
    if (*next_expire <= now_ms) {
        return 0;
    }

    constexpr UnixMillis max_wait_ms = kMaxMaintenanceIntervalMs;
    if (now_ms <= std::numeric_limits<UnixMillis>::max() - max_wait_ms &&
        *next_expire > now_ms + max_wait_ms) {
        return kMaxMaintenanceIntervalMs;
    }

    // The subtraction is safe here: either now is near UnixMillis::max(), or
    // the comparison above proved that the positive difference is <= 100 ms.
    return static_cast<int>(*next_expire - now_ms);
}

bool Server::shutdown_requested() const {
    return shutdown_flag_ != nullptr && *shutdown_flag_ != 0;
}

void Server::run() {
    setup_listen_socket();

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) handle_error("epoll_create1");

    struct epoll_event listen_event{};
    struct epoll_event events[kMaxEvents];
    listen_event.events = EPOLLIN;
    listen_event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &listen_event) == -1)
        handle_error("epoll_ctl listen_fd");

    std::cout << "[INFO] Event loop started." << std::endl;
    bool expire_work_remains = false;

    while (!shutdown_requested()) {
        // Maintenance always gets a bounded slice before the next I/O batch.
        const ExpireCycleResult expire_result = db_.activeExpireCycle(
            kExpireKeysPerCycle,
            std::chrono::duration_cast<std::chrono::microseconds>(kExpireRuntime));
        db_.advanceRehash(kExpireRuntime);
        expire_work_remains = expire_result.more_due;

        const int timeout_ms = epoll_timeout_ms(expire_work_remains);
        const int ready_count =
            epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            handle_error("epoll_wait");
        }

        for (int index = 0; index < ready_count; ++index) {
            const int fd = events[index].data.fd;
            if (fd == listen_fd_) {
                accept_client();
            } else {
                enqueue_client(fd, events[index].events);
            }
        }

        process_ready_clients();
    }

    std::cout << "[INFO] Shutdown requested; leaving event loop." << std::endl;
}
