// Database.hpp
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