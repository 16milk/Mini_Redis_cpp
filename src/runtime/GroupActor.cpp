#include "mini_redis/runtime/GroupActor.hpp"

#include "mini_redis/consensus/MemoryStorage.hpp"
#include "mini_redis/consensus/RaftCore.hpp"
#include "mini_redis/core/Database.hpp"
#include "mini_redis/persistence/PersistentStorage.hpp"

#include <utility>

namespace runtime {

namespace {

constexpr std::size_t kExpireKeysPerCycle = 64;
constexpr auto kExpireRuntime = std::chrono::milliseconds(1);

TaskPriority priorityFor(TrafficClass traffic) {
    if (isControlTraffic(traffic) || traffic == TrafficClass::kRaft) {
        return TaskPriority::kControl;
    }
    if (isBulkTraffic(traffic)) {
        return TaskPriority::kBulk;
    }
    return TaskPriority::kClient;
}

}  // namespace

GroupActor::GroupActor(consensus::GroupId id, Database& store,
                       cluster::ClusterRouter* router)
    : id_(id), store_(store), commands_(store, router) {}

GroupActor::~GroupActor() = default;

void GroupActor::attachRaft(std::unique_ptr<consensus::RaftCore> raft,
                            std::unique_ptr<consensus::MemoryStorage> memory) {
    raft_ = std::move(raft);
    memory_ = std::move(memory);
}

void GroupActor::attachWal(persistence::PersistentStorage* wal) { wal_ = wal; }

void GroupActor::setOutbound(std::function<void(consensus::Message)> outbound) {
    outbound_ = std::move(outbound);
}

bool GroupActor::enqueue(GroupTask task) {
    const IoLimits& limits = task.priority == TaskPriority::kControl
                                 ? raft_limits_
                                 : (task.priority == TaskPriority::kBulk ? bulk_limits_
                                                                         : client_limits_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (task.priority == TaskPriority::kClient) {
        if (client_.size() >= limits.mailbox_high_watermark ||
            queued_bytes_ + task.bytes >= limits.mailbox_bytes_high_watermark ||
            wal_pending_bytes_ >= limits.wal_bytes_high_watermark) {
            client_rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    } else if (task.priority == TaskPriority::kBulk) {
        if (bulk_.size() >= limits.mailbox_high_watermark ||
            queued_bytes_ + task.bytes >= limits.mailbox_bytes_high_watermark) {
            return false;
        }
    } else if (control_.size() >= limits.mailbox_high_watermark) {
        return false;
    }

    queued_bytes_ += task.bytes;
    switch (task.priority) {
        case TaskPriority::kControl:
            control_.push_back(std::move(task));
            break;
        case TaskPriority::kBulk:
            bulk_.push_back(std::move(task));
            break;
        case TaskPriority::kClient:
            client_.push_back(std::move(task));
            break;
    }
    return true;
}

bool GroupActor::submitClient(ClientWork work) {
    std::size_t bytes = 0;
    for (const auto& arg : work.args) {
        bytes += arg.size();
    }
    GroupTask task;
    task.priority = TaskPriority::kClient;
    task.traffic = TrafficClass::kClient;
    task.bytes = bytes;
    task.run = [this, work = std::move(work)]() mutable {
        finishClient(work, executeClient(work.args));
    };
    return enqueue(std::move(task));
}

bool GroupActor::submitRaftMessage(consensus::Message message) {
    GroupTask task;
    task.priority = TaskPriority::kControl;
    task.traffic = TrafficClass::kRaft;
    task.bytes = message.snapshot.data.size();
    for (const auto& entry : message.entries) {
        task.bytes += entry.payload.size();
    }
    task.run = [this, message = std::move(message)]() mutable {
        if (raft_) {
            raft_->step(std::move(message));
            driveRaft();
        }
    };
    return enqueue(std::move(task));
}

bool GroupActor::submitTick(std::int64_t now_ms) {
    GroupTask task;
    task.priority = TaskPriority::kControl;
    task.traffic = TrafficClass::kRaft;
    task.run = [this, now_ms]() {
        maintainStore();
        if (raft_) {
            raft_->tick(now_ms);
            driveRaft();
        }
    };
    return enqueue(std::move(task));
}

bool GroupActor::submitBulk(std::function<void()> work, std::size_t bytes,
                            TrafficClass traffic) {
    GroupTask task;
    task.priority = priorityFor(traffic);
    task.traffic = traffic;
    task.bytes = bytes;
    task.run = std::move(work);
    return enqueue(std::move(task));
}

void GroupActor::drain(std::size_t max_tasks, std::chrono::microseconds budget) {
    running_serial_.store(true, std::memory_order_release);
    serial_owner_ = std::this_thread::get_id();
    const auto started = std::chrono::steady_clock::now();
    std::size_t ran = 0;

    // Maintenance always gets a slice so expiry cannot starve behind bulk I/O.
    maintainStore();
    driveRaft();

    while (ran < max_tasks) {
        if (ran != 0 && std::chrono::steady_clock::now() - started >= budget) {
            break;
        }
        GroupTask task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task = popLocked();
            if (!task.run) {
                break;
            }
            if (queued_bytes_ >= task.bytes) {
                queued_bytes_ -= task.bytes;
            } else {
                queued_bytes_ = 0;
            }
        }
        if (task.run) {
            task.run();
        }
        ++ran;
        tasks_run_.fetch_add(1, std::memory_order_relaxed);
    }

    serial_owner_ = std::thread::id{};
    running_serial_.store(false, std::memory_order_release);
}

bool GroupActor::canAcceptClient() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_.size() < client_limits_.mailbox_high_watermark &&
           queued_bytes_ < client_limits_.mailbox_bytes_high_watermark &&
           wal_pending_bytes_ < client_limits_.wal_bytes_high_watermark;
}

bool GroupActor::clientBelowResume() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_.size() <= client_limits_.mailbox_low_watermark &&
           queued_bytes_ <= client_limits_.mailbox_bytes_low_watermark &&
           wal_pending_bytes_ <= client_limits_.wal_bytes_low_watermark;
}

MailboxSnapshot GroupActor::mailbox() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MailboxSnapshot snap;
    snap.control = control_.size();
    snap.client = client_.size();
    snap.bulk = bulk_.size();
    snap.bytes = queued_bytes_;
    snap.wal_pending_bytes = wal_pending_bytes_;
    return snap;
}

bool GroupActor::hasWork() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !control_.empty() || !client_.empty() || !bulk_.empty();
}

GroupTask GroupActor::popLocked() {
    auto take = [](std::deque<GroupTask>& queue) -> GroupTask {
        GroupTask task = std::move(queue.front());
        queue.pop_front();
        return task;
    };
    if (!control_.empty()) {
        return take(control_);
    }
    if (!client_.empty()) {
        return take(client_);
    }
    if (!bulk_.empty()) {
        return take(bulk_);
    }
    return {};
}

void GroupActor::driveRaft() {
    if (!raft_ || !raft_->hasReady()) {
        return;
    }
    consensus::Ready ready = raft_->takeReady();
    if (wal_ != nullptr) {
        wal_->applyReady(ready);
    } else if (memory_) {
        memory_->applyReady(ready);
    }
    wal_pending_bytes_ = 0;
    if (outbound_) {
        for (auto& message : ready.messages) {
            outbound_(std::move(message));
        }
    }
    for (const auto& entry : ready.committed) {
        raft_->reportApplied(entry.index);
    }
    raft_->advance();
}

void GroupActor::maintainStore() {
    store_.activeExpireCycle(
        kExpireKeysPerCycle,
        std::chrono::duration_cast<std::chrono::microseconds>(kExpireRuntime));
    store_.advanceRehash(kExpireRuntime);
}

void GroupActor::finishClient(const ClientWork& work, std::string response) {
    if (work.reply_to == nullptr) {
        return;
    }
    Completion completion;
    completion.connection_id = work.connection_id;
    completion.generation = work.generation;
    completion.request_seq = work.request_seq;
    completion.response = std::move(response);
    work.reply_to->push(std::move(completion));
}

std::string GroupActor::executeClient(const std::vector<std::string>& args) {
    return commands_.executeOnStore(args);
}

}  // namespace runtime
