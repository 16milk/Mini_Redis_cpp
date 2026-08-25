#include "mini_redis/core/Database.hpp"

#include "mini_redis/core/Dict.hpp"
#include "mini_redis/core/RedisObject.hpp"
#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include "mini_redis/persistence/Rdb.hpp"
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

bool Database::hget(const std::string& key, const std::string& field, std::string& out_value) const {
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
    if (list->size() == 0) data_.erase(key);
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
    if (list->size() == 0) data_.erase(key);
    return true;
}

bool Database::lindex(const std::string& key, long long index, std::string& out_value) const {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ListObject*>(obj.get())->get(index, out_value);
}

std::vector<std::string> Database::lrange(const std::string& key, long long start, long long stop) const {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::LIST) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ListObject*>(obj.get())->range(start, stop);
}

size_t Database::llen(const std::string& key) const {
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
    if (list->size() == 0) data_.erase(key);
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
    if (list->size() == 0) data_.erase(key);
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
    if (set->size() == 0) data_.erase(key);
    return removed;
}

bool Database::sismember(const std::string& key, const std::string& member) const {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::SET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const SetObject*>(obj.get())->contains(member);
}

std::vector<std::string> Database::smembers(const std::string& key) const {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::SET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const SetObject*>(obj.get())->members();
}

size_t Database::scard(const std::string& key) const {
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
    if (zset->size() == 0) data_.erase(key);
    return removed;
}

bool Database::zscore(const std::string& key, const std::string& member, double& out_score) const {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->get_score(member, out_score);
}

std::vector<std::string> Database::zrange(const std::string& key, long long start, long long stop,
                                          bool with_scores) const {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->range(start, stop, with_scores);
}

std::vector<std::string> Database::zrangebyscore(const std::string& key, double min, double max,
                                                 bool with_scores) const {
    auto obj = lookupKey(key);
    if (!obj) return {};
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->range_by_score(min, max, with_scores);
}

bool Database::zrank(const std::string& key, const std::string& member, size_t& out_rank) const {
    auto obj = lookupKey(key);
    if (!obj) return false;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->rank(member, out_rank);
}

size_t Database::zcard(const std::string& key) const {
    auto obj = lookupKey(key);
    if (!obj) return 0;
    if (obj->type() != ObjectType::ZSET) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return static_cast<const ZSetObject*>(obj.get())->size();
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
