// Server.cpp
#include "Server.hpp"
#include "utils.hpp"
#include "Protocol.hpp"
#include "Command.hpp"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>
#include <csignal>
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

    // 将 client_fd 加入 epoll 监听（目前只监听可读，虽不做处理）
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = client_fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
        std::cerr << "[WARN] epoll_ctl add client failed" << std::endl;
        connections_.erase(client_fd);
        close(client_fd);
        return;
    }

    std::cout << "[INFO] Client connected, fd=" << client_fd << std::endl;
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
            // 在 Server::run() 的 for 循环内，替换原来的 else 块：
            } else {
                auto it = connections_.find(fd);
                if (it == connections_.end()) continue;

                Connection* conn = it->second.get();

                // 1. 读取数据
                if (!conn->readFromSocket()) {
                    std::cout << "[INFO] Client disconnected, fd=" << fd << std::endl;
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    connections_.erase(it);
                    continue;
                }

                // 2. 尝试解析命令
                std::vector<std::string> args;
                RespParser parser;
                auto result = parser.parse(conn->getReadBuffer(), args);

                if (result == RespParser::ParseResult::COMPLETE) {
                // 3. 处理命令（阶段二：只处理 PING）
                    // 调用全局命令处理器
                    extern std::unique_ptr<CommandHandler> g_cmd_handler;
                    std::string response = g_cmd_handler->execute(args);
                    conn->sendResponse(response);

                    // 临时：清空整个缓冲区（阶段三仍用此简化方案）
                    conn->consumeInput(conn->getReadBuffer().size());
                } else if (result == RespParser::ParseResult::ERROR) {
                    conn->sendResponse(RespParser::encodeError("protocol error"));
                    conn->consumeInput(conn->getReadBuffer().size()); // 清空
                }
                // INCOMPLETE: 等待更多数据

                // 5. 尝试发送响应
                if (!conn->writeToSocket()) {
                    std::cout << "[INFO] Failed to write to client, closing fd=" << fd << std::endl;
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    connections_.erase(it);
                }
            }
        }
    }
}