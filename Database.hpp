// Database.hpp（阶段四升级版）
#pragma once
#include "RedisObject.hpp"
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

class Database {
public:
    Database(); // 默认构造，尝试加载 dump.rdb
    explicit Database(bool disable_rdb_load); // 可选：禁用加载（用于测试）

    // --- String Commands ---
    void set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& out_value) const;

    // --- Hash Commands ---
    void hset(const std::string& key, const std::string& field, const std::string& value);
    bool hget(const std::string& key, const std::string& field, std::string& out_value) const;

    // 辅助：检查 key 是否存在且类型匹配
    bool keyExists(const std::string& key) const;
    bool checkType(const std::string& key, ObjectType expected) const;

    // 删除 keys，返回实际删除的数量
    size_t del(const std::vector<std::string>& keys);

    // 检查 keys 是否存在，返回存在的数量
    size_t exists(const std::vector<std::string>& keys) const;

    // 返回所有 key（用于 KEYS *）
    std::vector<std::string> getAllKeys(const std::string& pattern) const;

    bool saveRdb(const std::string& filename = "dump.rdb") const;

private:
    std::shared_ptr<RedisObject> lookupKey(const std::string& key) const;
    void storeKey(const std::string& key, std::shared_ptr<RedisObject> obj);

    std::unordered_map<std::string, std::shared_ptr<RedisObject>> data_;
};