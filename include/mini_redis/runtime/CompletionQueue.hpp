#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace runtime {

// A finished group-actor result, routed back to the reactor that owns the
// connection. generation lets the reactor drop completions after fd reuse.
struct Completion {
    std::uint64_t connection_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t request_seq = 0;
    std::string response;
};

// Multi-producer (group actors) / single-consumer (owning reactor) queue.
// Optional self-pipe wakes an epoll loop without polling.
class CompletionQueue {
public:
    CompletionQueue() = default;
    ~CompletionQueue();
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    bool enableWakeup();
    int wakeupFd() const { return read_fd_; }

    void push(Completion item);
    std::vector<Completion> popAll();
    void consumeWakeup();
    void wake();

    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<Completion> items_;
    int read_fd_ = -1;
    int write_fd_ = -1;
};

}  // namespace runtime
