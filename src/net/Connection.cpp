#include "mini_redis/net/Connection.hpp"
#include "mini_redis/net/utils.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <limits>

Connection::Connection(int sockfd) : sockfd_(sockfd) {}

Connection::~Connection() {
    if (sockfd_ != -1) close(sockfd_);
}

bool Connection::readFromSocket() {
    return readFromSocket(std::numeric_limits<std::size_t>::max(),
                          std::numeric_limits<std::size_t>::max())
        .ok;
}

Connection::ReadResult Connection::readFromSocket(
    std::size_t max_bytes, std::size_t max_read_calls) {
    ReadResult result;
    char buf[4096];

    while (result.bytes_read < max_bytes && result.read_calls < max_read_calls) {
        const std::size_t remaining = max_bytes - result.bytes_read;
        const std::size_t request_size = std::min<std::size_t>(sizeof(buf), remaining);
        if (request_size == 0) {
            break;
        }

        ++result.read_calls;
        const ssize_t read_count = read(sockfd_, buf, request_size);
        if (read_count > 0) {
            read_buffer_.append(buf, static_cast<size_t>(read_count));
            result.bytes_read += static_cast<std::size_t>(read_count);
            continue;
        }
        if (read_count == 0) {
            // 对端关闭写方向；仍可能需要把已入队的响应写回。
            peer_read_closed_ = true;
            return result;
        }
        if (errno == EINTR) {
            --result.read_calls;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return result;
        }
        closed_ = true;
        result.ok = false;
        return result;
    }

    result.budget_exhausted = !peer_read_closed_ &&
        (result.bytes_read >= max_bytes || result.read_calls >= max_read_calls);
    return result;
}

void Connection::sendResponse(const std::string& resp) {
    constexpr std::size_t kCompactThreshold = 1024 * 1024;
    if (write_offset_ != 0 &&
        (write_offset_ >= kCompactThreshold || write_offset_ >= write_buffer_.size() / 2)) {
        write_buffer_.erase(0, write_offset_);
        write_offset_ = 0;
    }
    write_buffer_ += resp;
    // Actual socket writes are performed by writeToSocket() from the event loop.
}

bool Connection::writeToSocket() {
    return writeToSocket(std::numeric_limits<std::size_t>::max(),
                         std::numeric_limits<std::size_t>::max());
}

bool Connection::writeToSocket(std::size_t max_bytes, std::size_t max_write_calls) {
    std::size_t bytes_written = 0;
    std::size_t write_calls = 0;
    while (hasPendingWrite() && bytes_written < max_bytes &&
           write_calls < max_write_calls) {
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        send_flags = MSG_NOSIGNAL;
#endif
        const std::size_t remaining = max_bytes - bytes_written;
        const std::size_t request_size =
            std::min(write_buffer_.size() - write_offset_, remaining);
        ++write_calls;
        const ssize_t written =
            send(sockfd_, write_buffer_.data() + write_offset_, request_size, send_flags);
        if (written > 0) {
            write_offset_ += static_cast<std::size_t>(written);
            bytes_written += static_cast<std::size_t>(written);
            continue;
        }

        if (written < 0 && errno == EINTR) {
            --write_calls;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        closed_ = true;
        return false;
    }

    if (write_offset_ == write_buffer_.size()) {
        write_buffer_.clear();
        write_offset_ = 0;
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
