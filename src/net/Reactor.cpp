#include "mini_redis/net/Reactor.hpp"

#include "mini_redis/net/Protocol.hpp"
#include "mini_redis/net/utils.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <utility>

namespace {

constexpr int kMaxEvents = 128;
constexpr std::size_t kAcceptsPerWake = 32;
constexpr int kMaxIdleWaitMs = 100;
constexpr std::size_t kReadyClientsPerRound = 128;
constexpr auto kReadyRoundRuntime = std::chrono::milliseconds(5);
constexpr auto kGroupTickInterval = std::chrono::milliseconds(50);

int set_tcp_nodelay(int fd) {
    int on = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

}  // namespace

Reactor::Reactor(int id, runtime::GroupScheduler& scheduler,
                 CommandHandler& local_commands, cluster::ClusterRouter* router,
                 runtime::ResourcePools* pools,
                 volatile std::sig_atomic_t* shutdown_flag)
    : id_(id),
      scheduler_(scheduler),
      frontend_(scheduler, local_commands, router),
      pools_(pools),
      shutdown_flag_(shutdown_flag) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        handle_error("epoll_create1");
    }
    if (!completions_.enableWakeup()) {
        handle_error("completion wakeup pipe");
    }
    struct epoll_event wake{};
    wake.events = EPOLLIN;
    wake.data.fd = completions_.wakeupFd();
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, completions_.wakeupFd(), &wake) == -1) {
        handle_error("epoll_ctl wakeup");
    }
}

Reactor::~Reactor() {
    connections_.clear();
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
    }
}

void Reactor::addListenSocket(int fd, runtime::TrafficClass traffic) {
    listen_fds_[fd] = traffic;
    struct epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
        handle_error("epoll_ctl listen_fd");
    }
}

void Reactor::offerConnection(int fd, runtime::TrafficClass traffic) {
    {
        std::lock_guard<std::mutex> lock(inbound_mu_);
        inbound_.push_back(IncomingSocket{fd, traffic});
    }
    completions_.wake();
}

void Reactor::requestStop() { completions_.wake(); }

bool Reactor::shutdownRequested() const {
    return shutdown_flag_ != nullptr && *shutdown_flag_ != 0;
}

void Reactor::accept_listen(int listen_fd, runtime::TrafficClass traffic) {
    for (std::size_t accepted = 0; accepted < kAcceptsPerWake; ++accepted) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = accept(
            listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                std::cerr << "[WARN] accept failed: " << std::strerror(errno)
                          << std::endl;
            }
            return;
        }

        const int client_flags = fcntl(client_fd, F_GETFL, 0);
        if (client_flags == -1 ||
            fcntl(client_fd, F_SETFL, client_flags | O_NONBLOCK) == -1) {
            std::cerr << "[WARN] failed to make client nonblocking, fd=" << client_fd
                      << ": " << std::strerror(errno) << std::endl;
            close(client_fd);
            continue;
        }
        set_tcp_nodelay(client_fd);

        if (pools_ != nullptr && !pools_->tryAdmit(traffic)) {
            close(client_fd);
            continue;
        }

        Reactor* owner = this;
        if (assigner_) {
            Reactor* assigned = assigner_(traffic);
            if (assigned != nullptr) {
                owner = assigned;
            }
        }
        if (owner == this) {
            adopt_client(client_fd, traffic);
        } else {
            owner->offerConnection(client_fd, traffic);
        }
    }
}

void Reactor::adopt_client(int client_fd, runtime::TrafficClass traffic) {
    auto connection = std::make_unique<Connection>(client_fd, traffic);
    struct epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = client_fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
        std::cerr << "[WARN] epoll_ctl add client failed, fd=" << client_fd
                  << ": " << std::strerror(errno) << std::endl;
        if (pools_ != nullptr) {
            pools_->release(traffic);
        }
        return;
    }
    connection->setEpollInterest(event.events);
    connection_ids_[connection->id()] = client_fd;
    connections_.emplace(client_fd, std::move(connection));
}

void Reactor::close_client(int fd) {
    auto it = connections_.find(fd);
    runtime::TrafficClass traffic = runtime::TrafficClass::kClient;
    if (it != connections_.end()) {
        traffic = it->second->trafficClass();
        connection_ids_.erase(it->second->id());
    }
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    connections_.erase(fd);
    queued_clients_.erase(fd);
    pending_client_work_.erase(fd);
    close_after_write_.erase(fd);
    if (pools_ != nullptr) {
        pools_->release(traffic);
    }
}

bool Reactor::update_client_events(int fd, Connection& connection) {
    connection.updateReadPause();
    std::uint32_t desired = 0;
    if (!connection.peerReadClosed() && close_after_write_.count(fd) == 0 &&
        !connection.readsPaused()) {
        desired |= EPOLLIN | EPOLLRDHUP;
    } else if (!connection.peerReadClosed()) {
        desired |= EPOLLRDHUP;
    }
    if (connection.hasPendingWrite()) {
        desired |= EPOLLOUT;
    }
    if (desired == 0) {
        return false;
    }
    if (desired == connection.epollInterest()) {
        return true;
    }

    struct epoll_event event{};
    event.events = desired;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        std::cerr << "[WARN] epoll_ctl mod client failed, fd=" << fd << ": "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    connection.setEpollInterest(desired);
    return true;
}

void Reactor::apply_completions() {
    completions_.consumeWakeup();
    const auto completions = completions_.popAll();
    for (const auto& item : completions) {
        const auto id_it = connection_ids_.find(item.connection_id);
        if (id_it == connection_ids_.end()) {
            continue;
        }
        const int fd = id_it->second;
        auto conn_it = connections_.find(fd);
        if (conn_it == connections_.end()) {
            continue;
        }
        Connection& connection = *conn_it->second;
        if (connection.generation() != item.generation) {
            continue;
        }
        connection.pipeline().complete(item.request_seq, item.response);
        flush_pipeline(connection);
        connection.setDownstreamPaused(connection.pipeline().pauseReads());
        enqueue_client(fd, connection.hasPendingWrite() ? EPOLLOUT : 0);
    }
}

void Reactor::adopt_inbound() {
    std::vector<IncomingSocket> incoming;
    {
        std::lock_guard<std::mutex> lock(inbound_mu_);
        incoming.swap(inbound_);
    }
    for (const IncomingSocket& socket : incoming) {
        adopt_client(socket.fd, socket.traffic);
    }
}

void Reactor::resume_downstream() {
    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (const auto& item : connections_) {
        fds.push_back(item.first);
    }
    for (const int fd : fds) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            continue;
        }
        Connection& connection = *it->second;
        if (!connection.downstreamPaused()) {
            continue;
        }
        const bool group_ok =
            !scheduler_.hasGroup(runtime::kStandaloneGroupId) ||
            scheduler_.clientBelowResume(runtime::kStandaloneGroupId);
        if (!group_ok) {
            continue;
        }
        if (!connection.pipeline().belowResumeWatermark()) {
            continue;
        }
        if (connection.pendingWriteBytes() >= connection.limits().output_low_watermark ||
            connection.pendingInputBytes() >= connection.limits().input_low_watermark) {
            continue;
        }
        connection.setDownstreamPaused(false);
        enqueue_client(fd, 0, true);
    }
}

void Reactor::flush_pipeline(Connection& connection) {
    for (const std::string& response : connection.pipeline().takeReady()) {
        connection.sendResponse(response);
    }
}

Reactor::InputProcessResult Reactor::process_client_input(
    Connection& connection, std::size_t max_commands,
    std::chrono::microseconds max_runtime) {
    InputProcessResult outcome;
    RespParser parser;
    const auto started = std::chrono::steady_clock::now();
    runtime::RequestContext ctx{connection.session(), connection.pipeline(),
                                completions_, connection.id(),
                                connection.generation(), connection.trafficClass()};

    while (connection.hasPendingInput()) {
        if (outcome.commands_processed >= max_commands ||
            (outcome.commands_processed != 0 &&
             std::chrono::steady_clock::now() - started >= max_runtime)) {
            outcome.needs_more_processing = true;
            return outcome;
        }
        if (connection.shouldPauseReads() && outcome.commands_processed != 0) {
            outcome.pause_reads = true;
            outcome.needs_more_processing = connection.hasPendingInput();
            return outcome;
        }

        std::vector<std::string> arguments;
        std::size_t bytes_consumed = 0;
        const auto result =
            parser.parse(connection.getReadBuffer(), arguments, bytes_consumed);

        if (result == RespParser::INCOMPLETE) {
            if (connection.peerReadClosed()) {
                connection.pipeline().begin(connection.session().beginRequest());
                connection.pipeline().complete(
                    connection.session().currentRequestSeq(),
                    RespParser::encodeError("protocol error"));
                flush_pipeline(connection);
                connection.consumeInput(connection.getReadBuffer().size());
                outcome.protocol_error = true;
            }
            return outcome;
        }

        if (result == RespParser::ERROR) {
            connection.pipeline().begin(connection.session().beginRequest());
            connection.pipeline().complete(connection.session().currentRequestSeq(),
                                           RespParser::encodeError("protocol error"));
            flush_pipeline(connection);
            connection.consumeInput(connection.getReadBuffer().size());
            outcome.protocol_error = true;
            return outcome;
        }

        const auto handled = frontend_.handle(ctx, arguments);
        if (handled.action == runtime::FrontendAction::kHold) {
            outcome.pause_reads = true;
            outcome.needs_more_processing = false;
            return outcome;
        }
        connection.consumeInput(bytes_consumed);
        flush_pipeline(connection);
        ++outcome.commands_processed;
        outcome.pause_reads = outcome.pause_reads || handled.pause_reads;
        connection.setDownstreamPaused(handled.pause_reads);
        if (connection.pendingWriteBytes() >= connection.limits().output_high_watermark) {
            outcome.needs_more_processing = connection.hasPendingInput();
            return outcome;
        }
    }

    return outcome;
}

void Reactor::enqueue_client(int fd, std::uint32_t events, bool continue_read) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }
    auto& work = pending_client_work_[fd];
    work.events |= events;
    work.continue_read = work.continue_read || continue_read;
    if (!queued_clients_.insert(fd).second) {
        return;
    }
    readyQueueFor(it->second->trafficClass()).push_back(fd);
}

std::deque<int>& Reactor::readyQueueFor(runtime::TrafficClass traffic) {
    if (runtime::isControlTraffic(traffic)) {
        return ready_control_;
    }
    if (runtime::isBulkTraffic(traffic)) {
        return ready_bulk_;
    }
    return ready_client_;
}

void Reactor::process_ready_clients() {
    const auto round_started = std::chrono::steady_clock::now();
    std::size_t clients_processed = 0;

    auto pop_next = [this]() -> int {
        if (!ready_control_.empty()) {
            const int fd = ready_control_.front();
            ready_control_.pop_front();
            return fd;
        }
        if (!ready_client_.empty()) {
            const int fd = ready_client_.front();
            ready_client_.pop_front();
            return fd;
        }
        if (!ready_bulk_.empty()) {
            const int fd = ready_bulk_.front();
            ready_bulk_.pop_front();
            return fd;
        }
        return -1;
    };

    while (clients_processed < kReadyClientsPerRound &&
           std::chrono::steady_clock::now() - round_started < kReadyRoundRuntime) {
        const int fd = pop_next();
        if (fd < 0) {
            break;
        }
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
        const runtime::IoLimits& limits = connection.limits();
        ++clients_processed;

        if (work.events & EPOLLERR) {
            close_client(fd);
            continue;
        }

        bool continue_read = work.continue_read;
        const bool closing_after_write = close_after_write_.count(fd) != 0;
        connection.updateReadPause();
        const bool output_allows_commands =
            connection.pendingWriteBytes() < limits.output_low_watermark;
        const bool commands_deferred_for_output =
            !closing_after_write && !output_allows_commands &&
            connection.hasPendingInput();
        const bool socket_readable =
            work.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP);
        const bool below_read_limit =
            connection.pendingInputBytes() < limits.input_high_watermark;
        if (!closing_after_write && output_allows_commands &&
            !connection.peerReadClosed() && !connection.readsPaused() &&
            (socket_readable || continue_read) && below_read_limit) {
            const auto read_result = connection.readFromSocket(
                limits.read_bytes_per_turn, limits.read_calls_per_turn);
            if (!read_result.ok) {
                close_client(fd);
                continue;
            }
            continue_read = read_result.budget_exhausted;
        } else {
            continue_read = false;
        }

        InputProcessResult input_result;
        if (!closing_after_write && output_allows_commands &&
            !connection.readsPaused()) {
            input_result = process_client_input(
                connection, limits.commands_per_turn, limits.command_runtime_per_turn);
        } else if (closing_after_write) {
            connection.discardInput();
        }
        if (input_result.protocol_error) {
            close_after_write_.insert(fd);
        }
        const bool must_close_after_write = close_after_write_.count(fd) != 0;

        if ((work.events & EPOLLOUT) || connection.hasPendingWrite()) {
            if (!connection.writeToSocket(limits.write_bytes_per_turn,
                                          limits.write_calls_per_turn)) {
                close_client(fd);
                continue;
            }
        }

        if (connection.peerReadClosed() && !connection.hasPendingInput() &&
            !connection.hasPendingWrite() && connection.pipeline().inflight() == 0) {
            close_client(fd);
            continue;
        }
        if (must_close_after_write && !connection.hasPendingWrite() &&
            connection.pipeline().inflight() == 0) {
            close_client(fd);
            continue;
        }

        if (!update_client_events(fd, connection)) {
            close_client(fd);
            continue;
        }

        if (connection.pendingInputBytes() >= limits.input_high_watermark &&
            output_allows_commands && !input_result.needs_more_processing) {
            close_client(fd);
            continue;
        }

        const bool resume_socket_read =
            continue_read &&
            connection.pendingInputBytes() < limits.input_low_watermark &&
            !connection.readsPaused();
        const bool resume_buffered_commands =
            input_result.needs_more_processing &&
            connection.pendingWriteBytes() < limits.output_low_watermark &&
            !connection.readsPaused();
        const bool output_unblocked_buffered_input =
            commands_deferred_for_output &&
            connection.pendingWriteBytes() < limits.output_low_watermark &&
            connection.hasPendingInput();
        if (resume_socket_read || resume_buffered_commands ||
            output_unblocked_buffered_input) {
            enqueue_client(fd, 0, resume_socket_read);
        }
    }
}

int Reactor::epoll_timeout_ms() const {
    if (!ready_control_.empty() || !ready_client_.empty() || !ready_bulk_.empty()) {
        return 0;
    }
    return kMaxIdleWaitMs;
}

void Reactor::run() {
    struct epoll_event events[kMaxEvents];
    last_tick_ = std::chrono::steady_clock::now();
    std::cout << "[INFO] Reactor " << id_ << " event loop started." << std::endl;

    while (!shutdownRequested()) {
        apply_completions();
        adopt_inbound();
        resume_downstream();

        const auto now_steady = std::chrono::steady_clock::now();
        if (id_ == 0 && now_steady - last_tick_ >= kGroupTickInterval) {
            last_tick_ = now_steady;
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            for (const consensus::GroupId group_id : scheduler_.groupIds()) {
                scheduler_.submitTick(group_id, now_ms);
            }
        }

        const int timeout_ms = epoll_timeout_ms();
        const int ready_count = epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            handle_error("epoll_wait");
        }

        const int wakeup_fd = completions_.wakeupFd();
        for (int index = 0; index < ready_count; ++index) {
            const int fd = events[index].data.fd;
            if (fd == wakeup_fd) {
                continue;
            }
            const auto listen_it = listen_fds_.find(fd);
            if (listen_it != listen_fds_.end()) {
                accept_listen(fd, listen_it->second);
            } else {
                enqueue_client(fd, events[index].events);
            }
        }

        apply_completions();
        adopt_inbound();
        resume_downstream();
        process_ready_clients();
    }

    std::cout << "[INFO] Reactor " << id_ << " leaving event loop." << std::endl;
}
