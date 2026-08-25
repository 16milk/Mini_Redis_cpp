#pragma once

#include "mini_redis/core/RedisObject.hpp"

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

enum class ListEncoding {
    ZIPLIST,
    LINKEDLIST
};

enum class ListInsertPosition {
    BEFORE,
    AFTER
};

// 列表对象。小列表使用 vector，超过阈值或包含较大元素后升级为 deque。
class ListObject : public RedisObject {
public:
    static constexpr size_t ZIPLIST_MAX_ENTRIES = 512;
    static constexpr size_t ZIPLIST_MAX_ENTRY_SIZE = 64;

    ListObject();

    ObjectType type() const override { return ObjectType::LIST; }
    size_t memory_usage() const override;

    void push_front(std::string value);
    void push_back(std::string value);
    bool pop_front(std::string& out);
    bool pop_back(std::string& out);
    bool get(long long index, std::string& out) const;
    std::vector<std::string> range(long long start, long long stop) const;
    long long remove(long long count, const std::string& value);
    void trim(long long start, long long stop);
    bool insert(const std::string& pivot, const std::string& value,
                ListInsertPosition position);
    std::vector<std::string> values() const;
    size_t size() const;

    ListEncoding encoding() const { return encoding_; }

private:
    ListEncoding encoding_;
    std::variant<std::vector<std::string>, std::deque<std::string>> storage_;

    bool should_use_ziplist() const;
    void promote_to_linkedlist();

    std::vector<std::string>& get_ziplist();
    std::deque<std::string>& get_linkedlist();
    const std::vector<std::string>& get_ziplist() const;
    const std::deque<std::string>& get_linkedlist() const;

    static std::optional<size_t> normalize_index(long long index, size_t size);
    static std::optional<std::pair<size_t, size_t>> normalize_range(
        long long start, long long stop, size_t size);
};
