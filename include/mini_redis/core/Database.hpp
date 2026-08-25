#pragma once
#include <string>
#include <vector>
#include <memory>               // for std::shared_ptr
#include <unordered_map>        // for data_ storage

class RedisObject;
class StringObject;         // 实际可以不用，因为只通过 RedisObject* 使用
class HashObject;
class Dict;

enum class ObjectType;

class Database {
public:
    Database();
    explicit Database(bool disable_rdb_load);

    // --- String ---
    void set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& out_value) const;

    // --- Hash ---
    void hset(const std::string& key, const std::string& field, const std::string& value);
    bool hget(const std::string& key, const std::string& field, std::string& out_value) const;

    // --- List ---
    size_t lpush(const std::string& key, const std::vector<std::string>& values);
    size_t rpush(const std::string& key, const std::vector<std::string>& values);
    bool lpop(const std::string& key, std::string& out_value);
    bool rpop(const std::string& key, std::string& out_value);
    bool lindex(const std::string& key, long long index, std::string& out_value) const;
    std::vector<std::string> lrange(const std::string& key, long long start, long long stop) const;
    size_t llen(const std::string& key) const;
    long long lrem(const std::string& key, long long count, const std::string& value);
    void ltrim(const std::string& key, long long start, long long stop);

    // --- Set ---
    size_t sadd(const std::string& key, const std::vector<std::string>& members);
    size_t srem(const std::string& key, const std::vector<std::string>& members);
    bool sismember(const std::string& key, const std::string& member) const;
    std::vector<std::string> smembers(const std::string& key) const;
    size_t scard(const std::string& key) const;

    // --- Sorted set ---
    size_t zadd(const std::string& key,
                const std::vector<std::pair<double, std::string>>& score_members);
    size_t zrem(const std::string& key, const std::vector<std::string>& members);
    bool zscore(const std::string& key, const std::string& member, double& out_score) const;
    std::vector<std::string> zrange(const std::string& key, long long start, long long stop,
                                    bool with_scores) const;
    std::vector<std::string> zrangebyscore(const std::string& key, double min, double max,
                                           bool with_scores) const;
    bool zrank(const std::string& key, const std::string& member, size_t& out_rank) const;
    size_t zcard(const std::string& key) const;

    // --- Key management ---
    size_t del(const std::vector<std::string>& keys);
    size_t exists(const std::vector<std::string>& keys) const;
    std::vector<std::string> getAllKeys(const std::string& pattern = "*") const;
    bool keyExists(const std::string& key) const;
    bool checkType(const std::string& key, ObjectType expected) const;

    // --- Persistence ---
    bool saveRdb(const std::string& filename = "dump.rdb") const;

    // --- 后台任务支持 ---
    std::vector<Dict*> get_rehashing_dicts();

    // --- 内存统计 ---
    size_t memory_usage() const;

private:
    std::unordered_map<std::string, std::shared_ptr<RedisObject>> data_;

    std::shared_ptr<RedisObject> lookupKey(const std::string& key) const;
    void storeKey(const std::string& key, std::shared_ptr<RedisObject> obj);
};
