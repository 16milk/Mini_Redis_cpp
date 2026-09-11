#pragma once

#include "mini_redis/command/Command.hpp"
#include "mini_redis/consensus/RaftTypes.hpp"
#include "mini_redis/runtime/CompletionQueue.hpp"
#include "mini_redis/runtime/IoBudget.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Database;

namespace consensus {
class RaftCore;
class MemoryStorage;
}

namespace persistence {
class PersistentStorage;
}

namespace runtime {

enum class TaskPriority : std::uint8_t {
    kControl = 0,  // Raft heartbeat / vote / tick
    kClient = 1,   // user commands
    kBulk = 2,     // snapshot / migration
};

struct ClientWork {
    std::uint64_t connection_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t request_seq = 0;
    std::vector<std::string> args;
    CompletionQueue* reply_to = nullptr;
};

struct GroupTask {
    TaskPriority priority = TaskPriority::kClient;
    TrafficClass traffic = TrafficClass::kClient;
    std::size_t bytes = 0;
    std::function<void()> run;
};

struct MailboxSnapshot {
    std::size_t control = 0;
    std::size_t client = 0;
    std::size_t bulk = 0;
    std::size_t bytes = 0;
    std::size_t wal_pending_bytes = 0;

    std::size_t total() const { return control + client + bulk; }
};

// One Raft group / shard. Raft state, WAL apply order and ShardStore (Database)
// are mutated only while a scheduler worker holds this actor exclusively.
class GroupActor {
public:
    GroupActor(consensus::GroupId id, Database& store,
               cluster::ClusterRouter* router = nullptr);
    ~GroupActor();

    consensus::GroupId id() const { return id_; }
    Database& store() { return store_; }
    const Database& store() const { return store_; }

    void attachRaft(std::unique_ptr<consensus::RaftCore> raft,
                    std::unique_ptr<consensus::MemoryStorage> memory);
    void attachWal(persistence::PersistentStorage* wal);
    void setOutbound(std::function<void(consensus::Message)> outbound);
    void setClientLimits(IoLimits limits) { client_limits_ = std::move(limits); }

    consensus::RaftCore* raft() { return raft_.get(); }
    const consensus::RaftCore* raft() const { return raft_.get(); }

    bool enqueue(GroupTask task);
    bool submitClient(ClientWork work);
    bool submitRaftMessage(consensus::Message message);
    bool submitTick(std::int64_t now_ms);
    bool submitBulk(std::function<void()> work, std::size_t bytes = 0,
                    TrafficClass traffic = TrafficClass::kSnapshot);

    // Exclusive: called by at most one scheduler worker at a time.
    void drain(std::size_t max_tasks, std::chrono::microseconds budget);

    bool canAcceptClient() const;
    bool clientBelowResume() const;
    MailboxSnapshot mailbox() const;
    bool hasWork() const;

    std::uint64_t tasks_run() const { return tasks_run_.load(std::memory_order_relaxed); }
    std::uint64_t client_rejected() const {
        return client_rejected_.load(std::memory_order_relaxed);
    }

    // Test helper: true while drain() is on the stack of some thread.
    bool runningSerial() const { return running_serial_.load(std::memory_order_acquire); }

private:
    GroupTask popLocked();  // caller holds mutex_
    void driveRaft();
    void maintainStore();
    void finishClient(const ClientWork& work, std::string response);
    std::string executeClient(const std::vector<std::string>& args);

    consensus::GroupId id_ = 1;
    Database& store_;
    CommandHandler commands_;
    IoLimits client_limits_ = clientIoLimits();
    IoLimits raft_limits_ = raftIoLimits();
    IoLimits bulk_limits_ = bulkIoLimits();

    std::unique_ptr<consensus::RaftCore> raft_;
    std::unique_ptr<consensus::MemoryStorage> memory_;
    persistence::PersistentStorage* wal_ = nullptr;
    std::function<void(consensus::Message)> outbound_;

    mutable std::mutex mutex_;
    std::deque<GroupTask> control_;
    std::deque<GroupTask> client_;
    std::deque<GroupTask> bulk_;
    std::size_t queued_bytes_ = 0;
    std::size_t wal_pending_bytes_ = 0;

    std::atomic<bool> running_serial_{false};
    std::atomic<std::uint64_t> tasks_run_{0};
    std::atomic<std::uint64_t> client_rejected_{0};
    std::thread::id serial_owner_{};
};

}  // namespace runtime
