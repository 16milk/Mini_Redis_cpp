// Server.hpp
#pragma once
#include <unordered_map>
#include <memory>
#include "Connection.hpp"
#include "Database.hpp"

class Server {
public:
    Server(int port = 6380);
    ~Server();
    void run();  // 主事件循环

private:
    void setup_listen_socket();
    void accept_client();
    int createListenSocket(int port);

    int port_;
    int listen_fd_;
    int epoll_fd_;
    Database db_;  
    
    // 管理所有客户端连接：fd -> Connection
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
};