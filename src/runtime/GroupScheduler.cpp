#include "mini_redis/runtime/GroupScheduler.hpp"

#include <stdexcept>
#include <utility>

namespace runtime {

namespace {

constexpr std::size_t kTasksPerSlice = 64;
constexpr auto kSliceBudget = std::chrono::milliseconds(2);
constexpr auto kIdleWait = std::chrono::milliseconds(50);

}  // namespace

GroupScheduler::GroupScheduler(unsigned worker_count)
    : worker_count_(worker_count == 0 ? 1 : worker_count) {}

GroupScheduler::~GroupScheduler() { stop(); }

GroupActor& GroupScheduler::addGroup(consensus::GroupId id, Database& store,
                                     cluster::ClusterRouter* router) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (groups_.count(id) != 0) {
        throw std::invalid_argument("group actor already exists");
    }
    auto actor = std::make_unique<GroupActor>(id, store, router);
    GroupActor& ref = *actor;
    groups_.emplace(id, std::move(actor));
    return ref;
}

bool GroupScheduler::hasGroup(consensus::GroupId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return groups_.count(id) != 0;
}

GroupActor& GroupScheduler::group(consensus::GroupId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    GroupActor* actor = findGroup(id);
    if (actor == nullptr) {
        throw std::out_of_range("unknown group actor");
    }
    return *actor;
}

const GroupActor& GroupScheduler::group(consensus::GroupId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const GroupActor* actor = findGroup(id);
    if (actor == nullptr) {
        throw std::out_of_range("unknown group actor");
    }
    return *actor;
}

std::vector<consensus::GroupId> GroupScheduler::groupIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<consensus::GroupId> ids;
    ids.reserve(groups_.size());
    for (const auto& item : groups_) {
        ids.push_back(item.first);
    }
    return ids;
}

std::size_t GroupScheduler::groupCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return groups_.size();
}

void GroupScheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    stop_.store(false, std::memory_order_release);
    workers_.reserve(worker_count_);
    for (unsigned index = 0; index < worker_count_; ++index) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void GroupScheduler::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

bool GroupScheduler::post(consensus::GroupId id, GroupTask task) {
    GroupActor* actor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actor = findGroup(id);
    }
    if (actor == nullptr) {
        return false;
    }
    if (!actor->enqueue(std::move(task))) {
        return false;
    }
    notify(id);
    return true;
}

bool GroupScheduler::submitClient(consensus::GroupId id, ClientWork work) {
    GroupActor* actor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actor = findGroup(id);
    }
    if (actor == nullptr) {
        return false;
    }
    if (!actor->submitClient(std::move(work))) {
        return false;
    }
    notify(id);
    return true;
}

bool GroupScheduler::submitRaftMessage(consensus::GroupId id,
                                       consensus::Message message) {
    GroupActor* actor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actor = findGroup(id);
    }
    if (actor == nullptr) {
        return false;
    }
    if (!actor->submitRaftMessage(std::move(message))) {
        return false;
    }
    notify(id);
    return true;
}

bool GroupScheduler::submitTick(consensus::GroupId id, std::int64_t now_ms) {
    GroupActor* actor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actor = findGroup(id);
    }
    if (actor == nullptr) {
        return false;
    }
    if (!actor->submitTick(now_ms)) {
        return false;
    }
    notify(id);
    return true;
}

bool GroupScheduler::submitBulk(consensus::GroupId id, std::function<void()> work,
                                std::size_t bytes, TrafficClass traffic) {
    GroupActor* actor = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        actor = findGroup(id);
    }
    if (actor == nullptr) {
        return false;
    }
    if (!actor->submitBulk(std::move(work), bytes, traffic)) {
        return false;
    }
    notify(id);
    return true;
}

bool GroupScheduler::canAcceptClient(consensus::GroupId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const GroupActor* actor = findGroup(id);
    return actor != nullptr && actor->canAcceptClient();
}

bool GroupScheduler::clientBelowResume(consensus::GroupId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const GroupActor* actor = findGroup(id);
    return actor != nullptr && actor->clientBelowResume();
}

void GroupScheduler::notify(consensus::GroupId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    enqueueReady(id);
    cv_.notify_one();
}

bool GroupScheduler::waitIdle(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_until(lock, deadline, [this] {
        if (stop_.load(std::memory_order_acquire)) {
            return true;
        }
        if (!ready_.empty() || !busy_.empty()) {
            return false;
        }
        for (const auto& item : groups_) {
            if (item.second->hasWork()) {
                return false;
            }
        }
        return idle_workers_.load(std::memory_order_acquire) ==
               static_cast<unsigned>(workers_.size());
    });
}

void GroupScheduler::workerLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        consensus::GroupId id = 0;
        GroupActor* actor = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_workers_.fetch_add(1, std::memory_order_relaxed);
            cv_.notify_all();
            const bool woke = cv_.wait_for(lock, kIdleWait, [this] {
                return stop_.load(std::memory_order_acquire) || !ready_.empty();
            });
            idle_workers_.fetch_sub(1, std::memory_order_relaxed);
            if (stop_.load(std::memory_order_acquire)) {
                return;
            }
            if (!woke || ready_.empty()) {
                continue;
            }
            id = ready_.front();
            ready_.pop_front();
            queued_.erase(id);
            actor = findGroup(id);
            if (actor == nullptr) {
                continue;
            }
            busy_.insert(id);
        }

        actor->drain(kTasksPerSlice, std::chrono::duration_cast<std::chrono::microseconds>(
                                         kSliceBudget));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_.erase(id);
            if (actor->hasWork()) {
                enqueueReady(id);
            }
            cv_.notify_all();
        }
    }
}

void GroupScheduler::enqueueReady(consensus::GroupId id) {
    if (busy_.count(id) != 0) {
        return;
    }
    if (!queued_.insert(id).second) {
        return;
    }
    ready_.push_back(id);
}

GroupActor* GroupScheduler::findGroup(consensus::GroupId id) {
    const auto it = groups_.find(id);
    if (it == groups_.end()) {
        return nullptr;
    }
    return it->second.get();
}

const GroupActor* GroupScheduler::findGroup(consensus::GroupId id) const {
    const auto it = groups_.find(id);
    if (it == groups_.end()) {
        return nullptr;
    }
    return it->second.get();
}

}  // namespace runtime
