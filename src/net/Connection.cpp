#include "mini_redis/net/Connection.hpp"
#include "mini_redis/net/utils.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

Connection::Connection(int sockfd) : sockfd_(sockfd) {}

Connection::~Connection() {
    if (sockfd_ != -1) close(sockfd_);
}

bool Connection::readFromSocket() {
    char buf[1024];
    while (true) {
        const ssize_t read_count = read(sockfd_, buf, sizeof(buf));
        if (read_count > 0) {
            read_buffer_.append(buf, static_cast<size_t>(read_count));
            continue;
        }
        if (read_count == 0) {
            // 对端关闭写方向；仍可能需要把已入队的响应写回。
            peer_read_closed_ = true;
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        closed_ = true;
        return false;
    }
}

void Connection::sendResponse(const std::string& resp) {
    write_buffer_ += resp;
    // 实际发送在 writeToSocket() 中进行（由 Server 调用）
}

bool Connection::writeToSocket() {
    while (!write_buffer_.empty()) {
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        send_flags = MSG_NOSIGNAL;
#endif
        const ssize_t written = send(sockfd_, write_buffer_.data(), write_buffer_.size(), send_flags);
        if (written > 0) {
            write_buffer_.erase(0, static_cast<size_t>(written));
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        closed_ = true;
        return false;
    }

    return true;
}

void Connection::consumeInput(size_t n) {
    if (n >= read_buffer_.size()) {
        read_buffer_.clear();
    } else {
        read_buffer_.erase(0, n);
    }
}
