// Database.cpp
// Database.cpp
#include "Database.hpp"
#include "RedisObject.hpp"      // 定义 ObjectType, ObjectEncoding
#include "StringObject.hpp"     // 用于 dynamic_cast / make_shared
#include "HashObject.hpp"       // 同上
#include "Dict.hpp"             // 用于 get_rehashing_dicts()
#include "Rdb.hpp"
#include <stdexcept>

Database::Database() {
    // 尝试从 dump.rdb 恢复数据
    auto loaded_data = RdbEncoder::loadFromFile("dump.rdb");
    data_ = std::move(loaded_data);
}

Database::Database(bool disable_rdb_load) {
    if (!disable_rdb_load) {
        auto loaded_data = RdbEncoder::loadFromFile("dump.rdb");
        data_ = std::move(loaded_data);
    }
    // 否则 data_ 保持空
}

void Database::set(const std::string& key, const std::string& value) {
    auto obj = std::make_shared<StringObject>(value);
    storeKey(key, std::move(obj));
}

bool Database::get(const std::string& key, std::string& out_value) const {
    auto obj = lookupKey(key);
    if (!obj || obj->type() != ObjectType::STRING) {
        return false;
    }
    out_value = static_cast<StringObject*>(obj.get())->value();
    return true;
}

void Database::hset(const std::string& key, const std::string& field, const std::string& value) {
    auto obj = lookupKey(key);
    if (!obj) {
        // key 不存在，创建新 Hash
        auto hash_obj = std::make_shared<HashObject>();
        hash_obj->set_field(field, value);
        storeKey(key, hash_obj);
    } else {
        if (obj->type() != ObjectType::HASH) {
            throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        static_cast<HashObject*>(obj.get())->set_field(field, value);
    }
}

bool Database::hget(const std::string& key, const std::string& field, std::string& out_value) const {
    auto obj = lookupKey(key);
    if (!obj || obj->type() != ObjectType::HASH) {
        return false;
    }
    return static_cast<HashObject*>(obj.get())->get_field(field, out_value);
}

// --- 辅助函数 ---
std::shared_ptr<RedisObject> Database::lookupKey(const std::string& key) const {
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : nullptr;
}

void Database::storeKey(const std::string& key, std::shared_ptr<RedisObject> obj) {
    data_[key] = std::move(obj);
}

bool Database::keyExists(const std::string& key) const {
    return data_.count(key) > 0;
}

bool Database::checkType(const std::string& key, ObjectType expected) const {
    auto obj = lookupKey(key);
    return obj && obj->type() == expected;
}

// Database.cpp
std::vector<Dict*> Database::get_rehashing_dicts() {
    std::vector<Dict*> result;
    for (auto& [key, obj] : data_) {
        if (obj->type() == ObjectType::HASH) {
            auto* hash_obj = static_cast<HashObject*>(obj.get());
            if (hash_obj->encoding() == ObjectEncoding::HASHTABLE) {
                Dict& d = hash_obj->get_hashtable(); // 需要 non-const getter
                if (d.is_rehashing()) {
                    result.push_back(&d);
                }
            }
        }
        // TODO: 未来添加 SetObject、ZSetObject 的 rehash 检查
    }
    return result;
}

size_t Database::del(const std::vector<std::string>& keys) {
    size_t count = 0;
    for (const auto& key : keys) {
        if (data_.erase(key) > 0) {
            ++count;
        }
    }
    return count;
}

size_t Database::exists(const std::vector<std::string>& keys) const {
    size_t count = 0;
    for (const auto& key : keys) {
        if (data_.count(key) > 0) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> Database::getAllKeys(const std::string& pattern) const {
    // 阶段五：只支持 "*"
    if (pattern != "*") {
        // 可选：返回空 或 报错（Redis 支持 glob，我们暂不实现）
        return {};
    }
    std::vector<std::string> result;
    result.reserve(data_.size());
    for (const auto& pair : data_) {
        result.push_back(pair.first);
    }
    return result;
}

bool Database::saveRdb(const std::string& filename) const {
    return RdbEncoder::saveToFile(filename, data_);
}

size_t Database::memory_usage() const {
    size_t total = sizeof(*this) + data_.bucket_count() * sizeof(void*);
    for (const auto& [key, obj] : data_) {
        total += key.capacity(); // key 字符串内存
        total += obj->memory_usage();
    }
    return total;
}