#pragma once

#include "mini_redis/consensus/RaftTypes.hpp"
#include "mini_redis/runtime/GroupActor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Database;

namespace cluster {
class ClusterRouter;
}

namespace runtime {

// Fixed worker pool. Each group is an actor: its mailbox is drained by at most
// one worker at a time, so shard state needs no internal locks. Different
// groups run in parallel.
class GroupScheduler {
public:
    explicit GroupScheduler(unsigned worker_count = 1);
    ~GroupScheduler();

    GroupScheduler(const GroupScheduler&) = delete;
    GroupScheduler& operator=(const GroupScheduler&) = delete;

    GroupActor& addGroup(consensus::GroupId id, Database& store,
                         cluster::ClusterRouter* router = nullptr);
    bool hasGroup(consensus::GroupId id) const;
    GroupActor& group(consensus::GroupId id);
    const GroupActor& group(consensus::GroupId id) const;
    std::vector<consensus::GroupId> groupIds() const;

    void start();
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    bool post(consensus::GroupId id, GroupTask task);
    bool submitClient(consensus::GroupId id, ClientWork work);
    bool submitRaftMessage(consensus::GroupId id, consensus::Message message);
    bool submitTick(consensus::GroupId id, std::int64_t now_ms);
    bool submitBulk(consensus::GroupId id, std::function<void()> work,
                    std::size_t bytes = 0,
                    TrafficClass traffic = TrafficClass::kSnapshot);

    bool canAcceptClient(consensus::GroupId id) const;
    bool clientBelowResume(consensus::GroupId id) const;

    // Wake a sleeping worker so it will drain `id` if the mailbox is non-empty.
    void notify(consensus::GroupId id);

    bool waitIdle(std::chrono::milliseconds timeout);

    unsigned workerCount() const { return worker_count_; }
    std::size_t groupCount() const;

private:
    void workerLoop();
    void enqueueReady(consensus::GroupId id);
    GroupActor* findGroup(consensus::GroupId id);
    const GroupActor* findGroup(consensus::GroupId id) const;

    unsigned worker_count_ = 1;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<consensus::GroupId, std::unique_ptr<GroupActor>> groups_;
    std::deque<consensus::GroupId> ready_;
    std::unordered_set<consensus::GroupId> queued_;
    std::unordered_set<consensus::GroupId> busy_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> idle_workers_{0};
};

}  // namespace runtime
