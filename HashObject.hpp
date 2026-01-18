#pragma once
#include "RedisObject.hpp"
#include "Dict.hpp"
#include <variant>
#include <vector>
#include <string>
#include <utility>

enum class ObjectEncoding {
    ZIPLIST,
    HASHTABLE  // 原 LINKEDLIST 改为更通用的 HASHTABLE
};

using Ziplist = std::vector<std::pair<std::string, std::string>>;

// 哈希对象
class HashObject : public RedisObject {
public:
    static constexpr size_t ZIPLIST_MAX_ENTRIES = 512;
    static constexpr size_t ZIPLIST_MAX_ENTRY_SIZE = 64;

    HashObject();

    ObjectType type() const override { return ObjectType::HASH; }
    size_t memory_usage() const override;

    void set_field(std::string field, std::string value);
    bool get_field(const std::string& field, std::string& out_value) const;
    bool del_field(const std::string& field); // 可选：HDEL
    size_t size() const;
    ObjectEncoding encoding() const { return encoding_; }

    // 返回所有 field-value 对（只读）
    std::vector<std::pair<std::string, std::string>> get_all_fields() const;

    auto& get_hashtable() { return std::get<1>(storage_); }
    const auto& get_hashtable() const { return std::get<1>(storage_); }

    bool exists(const std::string& field) const; // 可选：HEXISTS

    void try_rehash_for_ms(int ms); // 主动推进接口

    Ziplist::iterator find_in_ziplist(const std::string& field);
    Ziplist::const_iterator find_in_ziplist(const std::string& field) const;

private:
    ObjectEncoding encoding_;
    std::variant<
        std::vector<std::pair<std::string, std::string>>, // ziplist: [field1, value1, field2, value2, ...]
        Dict      // hashtable
    > storage_;

    // 辅助函数
    bool should_use_ziplist() const;
    void promote_to_hashtable();

    // 安全访问 storage_
    Ziplist& get_ziplist() { return std::get<0>(storage_); }
    const Ziplist& get_ziplist() const { return std::get<0>(storage_); }
};