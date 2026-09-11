#include "mini_redis/runtime/CompletionQueue.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace runtime {

namespace {

void closeFd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

}  // namespace

CompletionQueue::~CompletionQueue() {
    closeFd(read_fd_);
    closeFd(write_fd_);
}

bool CompletionQueue::enableWakeup() {
    if (read_fd_ >= 0) {
        return true;
    }
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        return false;
    }
    for (int fd : fds) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            close(fds[0]);
            close(fds[1]);
            return false;
        }
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    return true;
}

void CompletionQueue::push(Completion item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(item));
    }
    wake();
}

std::vector<Completion> CompletionQueue::popAll() {
    std::vector<Completion> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(items_);
    return out;
}

void CompletionQueue::consumeWakeup() {
    if (read_fd_ < 0) {
        return;
    }
    char buffer[64];
    while (true) {
        const ssize_t n = read(read_fd_, buffer, sizeof(buffer));
        if (n > 0) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

void CompletionQueue::wake() {
    if (write_fd_ < 0) {
        return;
    }
    const char byte = 1;
    while (true) {
        const ssize_t written = write(write_fd_, &byte, 1);
        if (written >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

std::size_t CompletionQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

}  // namespace runtime
