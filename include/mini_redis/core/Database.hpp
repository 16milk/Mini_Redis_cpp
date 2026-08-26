#pragma once

#include "mini_redis/core/Expiration.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class RedisObject;
enum class ObjectType;

struct ExpireCycleResult {
    std::size_t examined = 0;
    std::size_t deleted = 0;
    bool more_due = false;
    std::optional<UnixMillis> next_deadline;
};

struct ExpirationStats {
    std::size_t expired_keys_total = 0;
    std::size_t lazy_expired_keys_total = 0;
    std::size_t active_expired_keys_total = 0;
    std::size_t active_expire_cycles_total = 0;
    std::size_t active_expire_budget_exhausted_total = 0;
};

class Database {
public:
    using NowFunction = std::function<UnixMillis()>;

    Database();
    explicit Database(bool disable_rdb_load);
    Database(bool disable_rdb_load, NowFunction now_function);

    // --- String ---
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, const std::string& value, UnixMillis ttl_ms);
    bool get(const std::string& key, std::string& out_value);

    // --- Hash ---
    std::size_t hset(
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& field_values);
    bool hget(const std::string& key, const std::string& field, std::string& out_value);

    // --- List ---
    std::size_t lpush(const std::string& key, const std::vector<std::string>& values);
    std::size_t rpush(const std::string& key, const std::vector<std::string>& values);
    bool lpop(const std::string& key, std::string& out_value);
    bool rpop(const std::string& key, std::string& out_value);
    bool lindex(const std::string& key, long long index, std::string& out_value);
    std::vector<std::string> lrange(const std::string& key, long long start, long long stop);
    std::size_t llen(const std::string& key);
    long long lrem(const std::string& key, long long count, const std::string& value);
    void ltrim(const std::string& key, long long start, long long stop);

    // --- Set ---
    std::size_t sadd(const std::string& key, const std::vector<std::string>& members);
    std::size_t srem(const std::string& key, const std::vector<std::string>& members);
    bool sismember(const std::string& key, const std::string& member);
    std::vector<std::string> smembers(const std::string& key);
    std::size_t scard(const std::string& key);

    // --- Sorted set ---
    std::size_t zadd(
        const std::string& key,
        const std::vector<std::pair<double, std::string>>& score_members);
    std::size_t zrem(const std::string& key, const std::vector<std::string>& members);
    bool zscore(const std::string& key, const std::string& member, double& out_score);
    std::vector<std::string> zrange(const std::string& key, long long start, long long stop,
                                    bool with_scores);
    std::vector<std::string> zrangebyscore(const std::string& key, double min, double max,
                                           bool with_scores);
    bool zrank(const std::string& key, const std::string& member, std::size_t& out_rank);
    std::size_t zcard(const std::string& key);

    // --- Key management and expiration ---
    std::size_t del(const std::vector<std::string>& keys);
    std::size_t exists(const std::vector<std::string>& keys);
    std::vector<std::string> getAllKeys(const std::string& pattern = "*");
    bool keyExists(const std::string& key);
    bool checkType(const std::string& key, ObjectType expected);
    bool expire(const std::string& key, UnixMillis ttl_ms);
    long long ttl(const std::string& key, bool milliseconds);
    bool persist(const std::string& key);

    // --- Persistence ---
    bool saveRdb(const std::string& filename = "dump.rdb") const;

    // --- Periodic maintenance ---
    ExpireCycleResult activeExpireCycle(
        std::size_t max_keys = 64,
        std::chrono::microseconds max_runtime = std::chrono::milliseconds(1));
    std::optional<UnixMillis> nextExpireAt() const;
    UnixMillis nowMs() const;
    std::size_t advanceRehash(std::chrono::milliseconds max_runtime);

    // --- Diagnostics ---
    std::size_t memory_usage() const;
    std::size_t expireKeyCount() const { return expires_.size(); }
    std::size_t scheduledExpireCount() const { return expire_schedule_.size(); }
    std::size_t physicalKeyCount() const { return data_.size(); }
    const ExpirationStats& expirationStats() const { return expiration_stats_; }
    bool validateExpirationIndexes() const;

private:
    using ObjectMap = std::unordered_map<std::string, std::shared_ptr<RedisObject>>;
    using ExpireSchedule = std::set<std::pair<UnixMillis, std::string>>;

    ObjectMap data_;
    ExpireMap expires_;
    ExpireSchedule expire_schedule_;
    NowFunction now_function_;
    ExpirationStats expiration_stats_;

    std::shared_ptr<RedisObject> lookupKey(const std::string& key);
    void storeKey(const std::string& key, std::shared_ptr<RedisObject> object);
    bool eraseKey(const std::string& key);
    bool expireIfNeeded(const std::string& key, UnixMillis now_ms);
    bool setExpireAt(const std::string& key, UnixMillis deadline_ms, UnixMillis now_ms);
    bool clearExpire(const std::string& key);
    void rebuildExpireSchedule();

    static UnixMillis systemNowMs();
    static UnixMillis checkedDeadline(UnixMillis now_ms, UnixMillis ttl_ms);
};
