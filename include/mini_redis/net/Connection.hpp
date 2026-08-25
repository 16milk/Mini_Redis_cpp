// Connection.hpp（更新版）
#pragma once
#include <string>
#include <memory>

class Connection {
public:
    explicit Connection(int sockfd);
    ~Connection();

    int get_fd() const { return sockfd_; }

    // 从 socket 读取数据到 read_buffer_
    bool readFromSocket();

    // 将响应加入 write_buffer_，并尝试发送
    void sendResponse(const std::string& resp);

    // 尝试将 write_buffer_ 中的数据写出
    bool writeToSocket();

    // 检查 read_buffer_ 是否包含完整命令
    bool hasCompleteCommand() const;

    // 获取当前读缓冲区内容（供 parser 使用）
    const std::string& getReadBuffer() const { return read_buffer_; }

    // 消费已解析的字节数（parser 成功后调用）
    void consumeInput(size_t n);

    // 检查连接是否应关闭（如对端关闭）
    bool shouldClose() const { return closed_; }

private:
    int sockfd_;
    std::string read_buffer_;
    std::string write_buffer_;
    bool closed_ = false;
};