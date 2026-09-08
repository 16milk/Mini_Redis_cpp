#pragma once

#include <cstdint>

namespace cluster {

// 连接级路由状态。ASKING 是一次性标志：它绑定在具体的请求序号上，
// 因此跨 shard 的异步完成顺序不会把它误用到另一个请求上，
// 连接断开后状态随之消失。
class ClientSession {
public:
    // 每条进入连接的命令取得一个递增序号。
    std::uint64_t beginRequest() { return ++request_seq_; }
    std::uint64_t currentRequestSeq() const { return request_seq_; }

    // ASKING 只对紧随其后的那一个已解析命令生效。
    void markAskingForNextRequest(std::uint64_t current_seq) {
        asking_seq_ = current_seq + 1;
    }
    bool askingFor(std::uint64_t seq) const {
        return asking_seq_ != 0 && asking_seq_ == seq;
    }
    bool askingPending() const { return asking_seq_ != 0; }
    void clearAsking() { asking_seq_ = 0; }

private:
    std::uint64_t request_seq_ = 0;
    std::uint64_t asking_seq_ = 0;
};

} // namespace cluster
