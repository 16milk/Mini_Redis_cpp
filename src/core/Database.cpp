#include "mini_redis/core/Database.hpp"

#include "mini_redis/core/Dict.hpp"
#include "mini_redis/core/RedisObject.hpp"
#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include "mini_redis/persistence/Rdb.hpp"
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

Database::Database() : Database(false, systemNowMs) {}

Database::Database(bool disable_rdb_load) : Database(disable_rdb_load, systemNowMs) {}

Database::Database(bool disable_rdb_load, NowFunction now_function)
    : now_function_(std::move(now_function)) {
    if (!now_function_) {
        now_function_ = systemNowMs;
    }
    if (!disable_rdb_load) {
        auto loaded = RdbEncoder::loadFromFile("dump.rdb", nowMs());
        if (!RdbEncoder::lastError().empty()) {
            throw std::runtime_error(RdbEncoder::lastError());
        }
        data_ = std::move(loaded.objects);
        expires_ = std::move(loaded.expires);
        rebuildExpireSchedule();
    }
}

void Database::set(const std::string& key, const std::string& value) {
    expireIfNeeded(key, nowMs());
    auto obj = std::make_shared<StringObject>(value);
    storeKey(key, std::move(obj));
}

void Database::set(const std::string& key, const std::string& value, UnixMillis ttl_ms) {
    if (ttl_ms <= 0) {
        throw std::invalid_argument("invalid expire time");
    }
    const UnixMillis now_ms = nowMs();
    const UnixMillis deadline_ms = checkedDeadline(now_ms, ttl_ms);
    expireIfNeeded(key, now_ms);
    auto object = std::make_shared<StringObject>(value);

    ExpireSchedule temporary_schedule;
    temporary_schedule.emplace(deadline_ms, key);
    ExpireSchedule::node_type schedule_node =
        temporary_schedule.extract(temporary_schedule.begin());

    ExpireMap::node_type expire_node;
    ObjectMap::node_type data_node;
    const auto data_it = data_.find(key);
    const bool had_data = data_it != data_.end();
    ExpireMap temporary_expires;
    temporary_expires.emplace(key, deadline_ms);
    expire_node = temporary_expires.extract(temporary_expires.begin());
    expires_.reserve(expires_.size() + 1);

    if (!had_data) {
        ObjectMap temporary_data;
        temporary_data.emplace(key, object);
        data_node = temporary_data.extract(temporary_data.begin());
        data_.reserve(data_.size() + 1);
    }

    clearExpire(key);
    expires_.insert(std::move(expire_node));
    expire_schedule_.insert(std::move(schedule_node));
    if (had_data) {
        data_it->second = std::move(object);
    } else {
        data_.insert(std::move(data_node));
    }
}

bool Database::get(const std::string& key, std::string& out_value) {
    auto obj = lookupKey(key);
    if (!obj) {
        return false;
    }
    if (obj->type() != ObjectType::STRING) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    out_value = static_cast<const StringObject*>(obj.get())->value();
    return true;
}

size_t Database::hset(
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& field_values) {
    auto obj = lookupKey(key);
    std::shared_ptr<HashObject> hash;
    if (!obj) {
        hash = std::make_shared<HashObject>();
        storeKey(key, hash);
    } else {
        if (obj->type() != ObjectType::HASH) {
            throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        hash = std::static_pointer_cast<HashObject>(obj);
    }

    size_t added = 0;
    for (const auto& [field, value] : field_values) {
        added += hash->set_field(field, value) ? 1 : 0;
    }
    return added;
}

bool Database::hget(const std::string& key, const std::string& field, std::string& out_value) {
    auto obj = lookupKey(key);
    if (!obj) {
        return false;
    }
    if (obj->type() != ObjectType::HASH) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const HashObject*>(obj.get())->get_field(field, out_value);
}

size_t Database::lpush(const std::string& key, const std::vector<std::string>& values) {
    auto obj = lookupKey(key);
    std::shared_ptr<ListObject> list;
    if (!obj) {
        list = std::make_shared<ListObject>();
        storeKey(key, list);
    } else {
        if (obj->type() != ObjectType::LIST) {
            throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        list = std::static_pointer_cast<ListObject>(obj);
    }
    for (const auto& value : values) {
        list->push_front(value);
    }
    return list->size();
}

size_t Database::rpush(const std::string& key, const std::vector<std::string>& values) {
    auto obj = lookupKey(key);
    std::shared_ptr<ListObject> list;
    if (!obj) {
        list = std::make_shared<ListObject>();
        storeKey(key, list);
    } else {
        if (obj->type() != ObjectType::LIST) {
            throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        list = std::static_pointer_cast<ListObject>(obj);
    }
    for (const auto& value : values) {
        list->push_back(value);
    }
    return list->size();
}

bool Database::lpop(const std::string& key, std::string& out_value) {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    auto* list = static_cast<ListObject*>(obj.get());
    if (!list->pop_front(out_value)) return false;
    if (list->size() == 0) eraseKey(key);
    return true;
}

bool Database::rpop(const std::string& key, std::string& out_value) {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    auto* list = static_cast<ListObject*>(obj.get());
    if (!list->pop_back(out_value)) return false;
    if (list->size() == 0) eraseKey(key);
    return true;
}

bool Database::lindex(const std::string& key, long long index, std::string& out_value) {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ListObject*>(obj.get())->get(index, out_value);
}

std::vector<std::string> Database::lrange(const std::string& key, long long start, long long stop) {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ListObject*>(obj.get())->range(start, stop);
}

size_t Database::llen(const std::string& key) {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ListObject*>(obj.get())->size();
}

long long Database::lrem(const std::string& key, long long count, const std::string& value) {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    auto* list = static_cast<ListObject*>(obj.get());
    const auto removed = list->remove(count, value);
    if (list->size() == 0) eraseKey(key);
    return removed;
}

void Database::ltrim(const std::string& key, long long start, long long stop) {
    auto obj = lookupKey(key);
    if (!obj) return;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    auto* list = static_cast<ListObject*>(obj.get());
    list->trim(start, stop);
    if (list->size() == 0) eraseKey(key);
}

size_t Database::sadd(const std::string& key, const std::vector<std::string>& members) {
    auto obj = lookupKey(key);
    std::shared_ptr<SetObject> set;
    if (!obj) {
        set = std::make_shared<SetObject>();
        storeKey(key, set);
    } else {
        if (obj->type() != ObjectType::SET) {
            throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        set = std::static_pointer_cast<SetObject>(obj);
    }
    size_t added = 0;
    for (const auto& member : members) {
        added += set->add(member) ? 1 : 0;
    }
    return added;
}

size_t Database::srem(const std::string& key, const std::vector<std::string>& members) {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::SET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    auto* set = static_cast<SetObject*>(obj.get());
    size_t removed = 0;
    for (const auto& member : members) {
        removed += set->remove(member) ? 1 : 0;
    }
    if (set->size() == 0) eraseKey(key);
    return removed;
}

bool Database::sismember(const std::string& key, const std::string& member) {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::SET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const SetObject*>(obj.get())->contains(member);
}

std::vector<std::string> Database::smembers(const std::string& key) {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::SET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const SetObject*>(obj.get())->members();
}

size_t Database::scard(const std::string& key) {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::SET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const SetObject*>(obj.get())->size();
}

size_t Database::zadd(
    const std::string& key, const std::vector<std::pair<double, std::string>>& score_members) {
    auto obj = lookupKey(key);
    std::shared_ptr<ZSetObject> zset;
    if (!obj) {
        zset = std::make_shared<ZSetObject>();
        storeKey(key, zset);
    } else {
        if (obj->type() != ObjectType::ZSET) {
            throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        zset = std::static_pointer_cast<ZSetObject>(obj);
    }
    size_t added = 0;
    for (const auto& [score, member] : score_members) {
        added += zset->add(score, member) ? 1 : 0;
    }
    return added;
}

size_t Database::zrem(const std::string& key, const std::vector<std::string>& members) {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    auto* zset = static_cast<ZSetObject*>(obj.get());
    size_t removed = 0;
    for (const auto& member : members) {
        removed += zset->remove(member) ? 1 : 0;
    }
    if (zset->size() == 0) eraseKey(key);
    return removed;
}

bool Database::zscore(const std::string& key, const std::string& member, double& out_score) {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->get_score(member, out_score);
}

std::vector<std::string> Database::zrange(const std::string& key, long long start, long long stop,
                                          bool with_scores) {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->range(start, stop, with_scores);
}

std::vector<std::string> Database::zrangebyscore(const std::string& key, double min, double max,
                                                 bool with_scores) {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->range_by_score(min, max, with_scores);
}

bool Database::zrank(const std::string& key, const std::string& member, size_t& out_rank) {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->rank(member, out_rank);
}

size_t Database::zcard(const std::string& key) {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->size();
}

// --- Key lifecycle and expiration helpers ---
std::shared_ptr<RedisObject> Database::lookupKey(const std::string& key) {
    expireIfNeeded(key, nowMs());
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : nullptr;
}

void Database::storeKey(const std::string& key, std::shared_ptr<RedisObject> obj) {
    auto data_it = data_.find(key);
    if (data_it == data_.end()) {
        data_.emplace(key, std::move(obj));
    } else {
        data_it->second = std::move(obj);
    }
    clearExpire(key);
}

bool Database::eraseKey(const std::string& key) {
    auto expire_it = expires_.find(key);
    if (expire_it != expires_.end()) {
        expire_schedule_.erase({expire_it->second, key});
        expires_.erase(expire_it);
    }
    return data_.erase(key) > 0;
}

bool Database::expireIfNeeded(const std::string& key, UnixMillis now_ms) {
    const auto expire_it = expires_.find(key);
    if (expire_it == expires_.end() || expire_it->second > now_ms) {
        return false;
    }
    eraseKey(key);
    ++expiration_stats_.expired_keys_total;
    ++expiration_stats_.lazy_expired_keys_total;
    return true;
}

bool Database::keyExists(const std::string& key) {
    return lookupKey(key) != nullptr;
}

bool Database::checkType(const std::string& key, ObjectType expected) {
    auto obj = lookupKey(key);
    return obj && obj->type() == expected;
}

size_t Database::del(const std::vector<std::string>& keys) {
    size_t count = 0;
    const UnixMillis now_ms = nowMs();
    for (const auto& key : keys) {
        if (expireIfNeeded(key, now_ms)) {
            continue;
        }
        if (eraseKey(key)) {
            ++count;
        }
    }
    return count;
}

size_t Database::exists(const std::vector<std::string>& keys) {
    size_t count = 0;
    const UnixMillis now_ms = nowMs();
    for (const auto& key : keys) {
        expireIfNeeded(key, now_ms);
        if (data_.count(key) != 0) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> Database::getAllKeys(const std::string& pattern) {
    // 阶段五：只支持 "*"
    if (pattern != "*") {
        // 可选：返回空 或 报错（Redis 支持 glob，我们暂不实现）
        return {};
    }
    const UnixMillis now_ms = nowMs();
    std::vector<std::string> expired_keys;
    std::vector<std::string> result;
    result.reserve(data_.size());
    for (const auto& [key, object] : data_) {
        (void)object;
        const auto expire_it = expires_.find(key);
        if (expire_it != expires_.end() && expire_it->second <= now_ms) {
            expired_keys.push_back(key);
        } else {
            result.push_back(key);
        }
    }
    for (const auto& key : expired_keys) {
        expireIfNeeded(key, now_ms);
    }
    return result;
}

bool Database::saveRdb(const std::string& filename) const {
    return RdbEncoder::saveToFile(filename, data_, expires_, nowMs());
}

bool Database::expire(const std::string& key, UnixMillis ttl_ms) {
    const UnixMillis now_ms = nowMs();
    std::optional<UnixMillis> deadline_ms;
    if (ttl_ms > 0) {
        deadline_ms = checkedDeadline(now_ms, ttl_ms);
    }
    if (expireIfNeeded(key, now_ms) || data_.count(key) == 0) {
        return false;
    }
    if (ttl_ms <= 0) {
        eraseKey(key);
        return true;
    }
    return setExpireAt(key, *deadline_ms, now_ms);
}

long long Database::ttl(const std::string& key, bool milliseconds) {
    const UnixMillis now_ms = nowMs();
    if (expireIfNeeded(key, now_ms) || data_.count(key) == 0) {
        return -2;
    }
    const auto expire_it = expires_.find(key);
    if (expire_it == expires_.end()) {
        return -1;
    }
    const UnixMillis deadline_ms = expire_it->second;
    if (now_ms < 0 &&
        deadline_ms > std::numeric_limits<UnixMillis>::max() + now_ms) {
        return milliseconds ? std::numeric_limits<long long>::max()
                            : std::numeric_limits<long long>::max() / 1000;
    }
    const UnixMillis remaining_ms = deadline_ms - now_ms;
    if (milliseconds) {
        return remaining_ms;
    }
    return remaining_ms / 1000 + (remaining_ms % 1000 >= 500 ? 1 : 0);
}

bool Database::persist(const std::string& key) {
    const UnixMillis now_ms = nowMs();
    if (expireIfNeeded(key, now_ms) || data_.count(key) == 0) {
        return false;
    }
    return clearExpire(key);
}

bool Database::setExpireAt(const std::string& key, UnixMillis deadline_ms,
                           UnixMillis now_ms) {
    if (expireIfNeeded(key, now_ms) || data_.count(key) == 0) {
        return false;
    }
    if (deadline_ms <= now_ms) {
        eraseKey(key);
        return true;
    }

    auto expire_it = expires_.find(key);
    const bool had_expire = expire_it != expires_.end();
    const UnixMillis old_deadline = had_expire ? expire_it->second : 0;
    if (had_expire && old_deadline == deadline_ms) {
        return true;
    }

    const auto schedule_result = expire_schedule_.emplace(deadline_ms, key);
    try {
        if (had_expire) {
            expire_it->second = deadline_ms;
        } else {
            expires_.emplace(key, deadline_ms);
        }
    } catch (...) {
        if (schedule_result.second) {
            expire_schedule_.erase(schedule_result.first);
        }
        throw;
    }
    if (had_expire) {
        expire_schedule_.erase({old_deadline, key});
    }
    return true;
}

bool Database::clearExpire(const std::string& key) {
    const auto expire_it = expires_.find(key);
    if (expire_it == expires_.end()) {
        return false;
    }
    expire_schedule_.erase({expire_it->second, key});
    expires_.erase(expire_it);
    return true;
}

ExpireCycleResult Database::activeExpireCycle(
    std::size_t max_keys, std::chrono::microseconds max_runtime) {
    ExpireCycleResult result;
    ++expiration_stats_.active_expire_cycles_total;
    const UnixMillis now_ms = nowMs();
    const auto started = std::chrono::steady_clock::now();

    while (!expire_schedule_.empty()) {
        const auto first = expire_schedule_.begin();
        if (first->first > now_ms || result.examined >= max_keys ||
            std::chrono::steady_clock::now() - started >= max_runtime) {
            break;
        }

        const UnixMillis deadline_ms = first->first;
        const std::string key = first->second;
        ++result.examined;
        const auto expire_it = expires_.find(key);
        if (expire_it == expires_.end() || expire_it->second != deadline_ms) {
            expire_schedule_.erase(first);
            continue;
        }
        eraseKey(key);
        ++result.deleted;
        ++expiration_stats_.expired_keys_total;
        ++expiration_stats_.active_expired_keys_total;
    }

    if (!expire_schedule_.empty()) {
        result.next_deadline = expire_schedule_.begin()->first;
        result.more_due = *result.next_deadline <= now_ms;
    }
    if (result.more_due &&
        (result.examined >= max_keys ||
         std::chrono::steady_clock::now() - started >= max_runtime)) {
        ++expiration_stats_.active_expire_budget_exhausted_total;
    }
    return result;
}

std::optional<UnixMillis> Database::nextExpireAt() const {
    if (expire_schedule_.empty()) {
        return std::nullopt;
    }
    return expire_schedule_.begin()->first;
}

UnixMillis Database::nowMs() const {
    return now_function_();
}

UnixMillis Database::systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

UnixMillis Database::checkedDeadline(UnixMillis now_ms, UnixMillis ttl_ms) {
    if ((ttl_ms > 0 && now_ms > std::numeric_limits<UnixMillis>::max() - ttl_ms) ||
        (ttl_ms < 0 && now_ms < std::numeric_limits<UnixMillis>::min() - ttl_ms)) {
        throw std::overflow_error("expire time overflow");
    }
    const UnixMillis deadline_ms = now_ms + ttl_ms;
    if (deadline_ms < 0) {
        throw std::overflow_error("expire time is before the Unix epoch");
    }
    return deadline_ms;
}

void Database::rebuildExpireSchedule() {
    expire_schedule_.clear();
    for (const auto& [key, deadline_ms] : expires_) {
        if (data_.count(key) == 0) {
            throw std::runtime_error("RDB expiration references a missing key");
        }
        expire_schedule_.emplace(deadline_ms, key);
    }
}

std::size_t Database::advanceRehash(std::chrono::milliseconds max_runtime) {
    const auto started = std::chrono::steady_clock::now();
    const UnixMillis now_ms = nowMs();
    std::size_t advanced = 0;
    for (auto data_it = data_.begin(); data_it != data_.end(); ++data_it) {
        if (std::chrono::steady_clock::now() - started >= max_runtime) {
            break;
        }
        const auto expire_it = expires_.find(data_it->first);
        if (expire_it != expires_.end() && expire_it->second <= now_ms) {
            continue;
        }
        auto& object = data_it->second;
        if (object->type() != ObjectType::HASH) {
            continue;
        }
        auto* hash = static_cast<HashObject*>(object.get());
        if (hash->encoding() == ObjectEncoding::HASHTABLE &&
            hash->get_hashtable().is_rehashing()) {
            hash->try_rehash_for_ms(1);
            ++advanced;
        }
    }
    return advanced;
}

bool Database::validateExpirationIndexes() const {
    if (expires_.size() != expire_schedule_.size()) {
        return false;
    }
    for (const auto& [key, deadline_ms] : expires_) {
        if (data_.count(key) == 0 ||
            expire_schedule_.count({deadline_ms, key}) != 1) {
            return false;
        }
    }
    return true;
}

size_t Database::memory_usage() const {
    size_t total = sizeof(*this) + data_.bucket_count() * sizeof(void*) +
                   expires_.bucket_count() * sizeof(void*);
    for (const auto& [key, obj] : data_) {
        total += key.capacity(); // key 字符串内存
        total += obj->memory_usage();
    }
    for (const auto& [key, deadline_ms] : expires_) {
        (void)deadline_ms;
        total += sizeof(ExpireMap::value_type) + key.capacity();
    }
    for (const auto& [deadline_ms, key] : expire_schedule_) {
        (void)deadline_ms;
        total += sizeof(ExpireSchedule::value_type) + key.capacity();
    }
    return total;
}
