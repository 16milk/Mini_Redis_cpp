#include "mini_redis/net/Server.hpp"

#include "mini_redis/command/Command.hpp"
#include "mini_redis/net/Protocol.hpp"
#include "mini_redis/net/utils.hpp"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

constexpr int MAX_EVENTS = 128;
constexpr int BACKLOG = 128;

Server::Server(int port) : port_(port), listen_fd_(-1), epoll_fd_(-1) {}

Server::~Server() {
    if (listen_fd_ != -1) close(listen_fd_);
    if (epoll_fd_ != -1) close(epoll_fd_);
}

int Server::createListenSocket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sockfd, 128);
    return sockfd;
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

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) == -1)
        handle_error("bind");

    if (listen(listen_fd_, BACKLOG) == -1)
        handle_error("listen");

    std::cout << "[INFO] Server listening on port " << port_ << std::endl;
}

void Server::accept_client() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "[WARN] accept failed: " << std::strerror(errno) << std::endl;
        return;
    }

    set_nonblocking(client_fd);

    // 创建 Connection 并加入管理
    auto conn = std::make_unique<Connection>(client_fd);
    connections_[client_fd] = std::move(conn);

    // 初始只监听可读；有待发送数据时 update_client_events 会额外启用 EPOLLOUT。
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = client_fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        std::cerr << "[WARN] epoll_ctl add client failed" << std::endl;
        connections_.erase(client_fd);
        close(client_fd);
        return;
    }

    std::cout << "[INFO] Client connected, fd=" << client_fd << std::endl;
}

void Server::close_client(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    connections_.erase(fd);
}

bool Server::update_client_events(int fd, const Connection& connection) {
    struct epoll_event ev{};
    if (!connection.peerReadClosed()) {
        ev.events |= EPOLLIN | EPOLLRDHUP;
    }
    if (connection.hasPendingWrite()) {
        ev.events |= EPOLLOUT;
    }
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
        std::cerr << "[WARN] epoll_ctl mod client failed, fd=" << fd
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    return true;
}

void Server::process_client_input(Connection& connection) {
    RespParser parser;
    while (!connection.getReadBuffer().empty()) {
        std::vector<std::string> args;
        size_t bytes_consumed = 0;
        const auto result = parser.parse(connection.getReadBuffer(), args, bytes_consumed);

        if (result == RespParser::INCOMPLETE) {
            return;
        }

        if (result == RespParser::ERROR) {
            connection.sendResponse(RespParser::encodeError("protocol error"));
            // 对畸形请求不存在可靠的帧边界；丢弃现有输入以便连接可继续使用。
            connection.consumeInput(connection.getReadBuffer().size());
            return;
        }

        extern std::unique_ptr<CommandHandler> g_cmd_handler;
        connection.sendResponse(g_cmd_handler->execute(args));
        connection.consumeInput(bytes_consumed);
    }
}

void Server::run() {
    setup_listen_socket();

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) handle_error("epoll_create1");

    struct epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) == -1)
        handle_error("epoll_ctl listen_fd");

    std::cout << "[INFO] Event loop started." << std::endl;

    while (true) {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue; // 被信号中断，继续
            handle_error("epoll_wait");
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                accept_client();
            } else {
                auto it = connections_.find(fd);
                if (it == connections_.end()) {
                    continue;
                }

                Connection* conn = it->second.get();
                const uint32_t event_flags = events[i].events;

                if (event_flags & EPOLLERR) {
                    std::cout << "[INFO] Client error, fd=" << fd << std::endl;
                    close_client(fd);
                    continue;
                }

                if (!conn->peerReadClosed() &&
                    (event_flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) {
                    if (!conn->readFromSocket()) {
                        std::cout << "[INFO] Client read failed, fd=" << fd << std::endl;
                        close_client(fd);
                        continue;
                    }
                    process_client_input(*conn);
                }

                if ((event_flags & EPOLLOUT) || conn->hasPendingWrite()) {
                    if (!conn->writeToSocket()) {
                        std::cout << "[INFO] Failed to write to client, closing fd=" << fd << std::endl;
                        close_client(fd);
                        continue;
                    }
                }

                if (conn->peerReadClosed() && !conn->hasPendingWrite()) {
                    std::cout << "[INFO] Client disconnected, fd=" << fd << std::endl;
                    close_client(fd);
                    continue;
                }

                if (!update_client_events(fd, *conn)) {
                    std::cout << "[INFO] Failed to write to client, closing fd=" << fd << std::endl;
                    close_client(fd);
                }
            }
        }
    }
}
