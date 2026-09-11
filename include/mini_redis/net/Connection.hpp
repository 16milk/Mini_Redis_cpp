#pragma once

#include "mini_redis/cluster/ClientSession.hpp"
#include "mini_redis/runtime/IoBudget.hpp"
#include "mini_redis/runtime/PipelineReorder.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

class Connection {
public:
    struct ReadResult {
        bool ok = true;
        bool budget_exhausted = false;
        std::size_t bytes_read = 0;
        std::size_t read_calls = 0;
    };

    explicit Connection(int sockfd,
                        runtime::TrafficClass traffic = runtime::TrafficClass::kClient);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    int get_fd() const { return sockfd_; }
    std::uint64_t id() const { return id_; }
    std::uint64_t generation() const { return generation_; }

    runtime::TrafficClass trafficClass() const { return traffic_; }
    void setTrafficClass(runtime::TrafficClass traffic);

    const runtime::IoLimits& limits() const { return limits_; }
    runtime::PipelineReorder& pipeline() { return pipeline_; }
    const runtime::PipelineReorder& pipeline() const { return pipeline_; }

    std::uint32_t epollInterest() const { return epoll_interest_; }
    void setEpollInterest(std::uint32_t interest) { epoll_interest_ = interest; }

    // 从 socket 读取数据到 read_buffer_。对端半关闭时保留已读数据和待写响应。
    bool readFromSocket();

    // 有预算地读取，供事件循环在持续可读连接间公平调度。
    ReadResult readFromSocket(std::size_t max_bytes, std::size_t max_read_calls);

    // 将响应加入 write_buffer_，并尝试发送
    void sendResponse(const std::string& resp);

    // 尽可能将 write_buffer_ 中的数据写出；遇到 EAGAIN 时保留未写内容。
    bool writeToSocket();
    // 有预算地写出；通过偏移推进，避免反复 erase 前缀造成 O(n^2) 搬移。
    bool writeToSocket(std::size_t max_bytes, std::size_t max_write_calls);

    // 获取当前读缓冲区内容（供 parser 使用）
    const std::string& getReadBuffer() const { return read_buffer_; }

    // 消费已解析的字节数（parser 成功后调用）
    void consumeInput(size_t n);

    // write buffer 非空时，服务端应订阅 EPOLLOUT。
    bool hasPendingWrite() const { return write_offset_ < write_buffer_.size(); }
    std::size_t pendingWriteBytes() const { return write_buffer_.size() - write_offset_; }
    bool hasPendingInput() const { return !read_buffer_.empty(); }
    std::size_t pendingInputBytes() const { return read_buffer_.size(); }
    void discardInput() { read_buffer_.clear(); }

    bool shouldPauseReads() const;
    bool shouldResumeReads() const;
    void setDownstreamPaused(bool paused) { downstream_paused_ = paused; }
    bool downstreamPaused() const { return downstream_paused_; }
    bool readsPaused() const { return reads_paused_; }
    void updateReadPause();

    // 检查连接是否应关闭（如对端关闭）
    bool shouldClose() const { return closed_; }
    bool peerReadClosed() const { return peer_read_closed_; }

    // 连接级路由状态（请求序号、一次性 ASKING）。随连接销毁而消失。
    cluster::ClientSession& session() { return session_; }
    const cluster::ClientSession& session() const { return session_; }

private:
    int sockfd_;
    std::uint64_t id_ = 0;
    std::uint64_t generation_ = 1;
    runtime::TrafficClass traffic_ = runtime::TrafficClass::kClient;
    runtime::IoLimits limits_{};
    runtime::PipelineReorder pipeline_;
    std::uint32_t epoll_interest_ = 0;
    std::string read_buffer_;
    std::string write_buffer_;
    std::size_t write_offset_ = 0;
    bool closed_ = false;
    bool peer_read_closed_ = false;
    bool reads_paused_ = false;
    bool downstream_paused_ = false;
    cluster::ClientSession session_;
};
