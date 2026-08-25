#pragma once

#include <string>

class Connection {
public:
    explicit Connection(int sockfd);
    ~Connection();

    int get_fd() const { return sockfd_; }

    // 从 socket 读取数据到 read_buffer_。对端半关闭时保留已读数据和待写响应。
    bool readFromSocket();

    // 将响应加入 write_buffer_，并尝试发送
    void sendResponse(const std::string& resp);

    // 尽可能将 write_buffer_ 中的数据写出；遇到 EAGAIN 时保留未写内容。
    bool writeToSocket();

    // 获取当前读缓冲区内容（供 parser 使用）
    const std::string& getReadBuffer() const { return read_buffer_; }

    // 消费已解析的字节数（parser 成功后调用）
    void consumeInput(size_t n);

    // write buffer 非空时，服务端应订阅 EPOLLOUT。
    bool hasPendingWrite() const { return !write_buffer_.empty(); }

    // 检查连接是否应关闭（如对端关闭）
    bool shouldClose() const { return closed_; }
    bool peerReadClosed() const { return peer_read_closed_; }

private:
    int sockfd_;
    std::string read_buffer_;
    std::string write_buffer_;
    bool closed_ = false;
    bool peer_read_closed_ = false;
};
