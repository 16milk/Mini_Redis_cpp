// Connection.cpp
#include "Connection.hpp"
#include "utils.hpp"
#include <unistd.h>
#include <cerrno>
#include <cstring>

Connection::Connection(int sockfd) : sockfd_(sockfd) {}

Connection::~Connection() {
    if (sockfd_ != -1) close(sockfd_);
}

bool Connection::readFromSocket() {
    char buf[1024];
    ssize_t n;
    while ((n = read(sockfd_, buf, sizeof(buf))) > 0) {
        read_buffer_.append(buf, n);
    }
    if (n == 0) {
        // 对端关闭
        closed_ = true;
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true; // 正常，无更多数据
        }
        // 其他错误
        closed_ = true;
        return false;
    }
    return true;
}

void Connection::sendResponse(const std::string& resp) {
    write_buffer_ += resp;
    // 实际发送在 writeToSocket() 中进行（由 Server 调用）
}

bool Connection::writeToSocket() {
    if (write_buffer_.empty()) return true;

    ssize_t n = write(sockfd_, write_buffer_.data(), write_buffer_.size());
    if (n <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true; // 下次再试
        }
        closed_ = true;
        return false;
    }

    // 移除已发送部分
    write_buffer_.erase(0, n);
    return true;
}

bool Connection::hasCompleteCommand() const {
    return !read_buffer_.empty();
}

void Connection::consumeInput(size_t n) {
    if (n >= read_buffer_.size()) {
        read_buffer_.clear();
    } else {
        read_buffer_.erase(0, n);
    }
}