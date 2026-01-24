// SetObject.hpp
#pragma once
#include "RedisObject.hpp"
#include "intset.hpp"  // 你的 intset 实现
#include <unordered_set>
#include <variant>
#include <vector>
#include <string>
#include <optional>

enum class ObjectEncoding {
    INTSET,
    STRSET  // 对应 unordered_set<std::string>
};

class SetObject : public RedisObject {
public:
    static constexpr size_t INTSET_MAX_ENTRIES = 512;

    SetObject();

    size_t memory_usage() const override;

    // 核心操作
    bool add(const std::string& member);
    bool remove(const std::string& member);
    bool contains(const std::string& member) const;
    size_t size() const;
    ObjectEncoding encoding() const { return encoding_; }

    // 返回所有成员（统一为 string）
    std::vector<std::string> members() const;

    // 安全访问内部结构（用于调试或 RDB）
    const intset& get_intset() const { return std::get<0>(storage_); }
    const std::unordered_set<std::string>& get_strset() const { return std::get<1>(storage_); }

private:
    static std::optional<int64_t> tryParseInt(const std::string& s);
    void promote_to_strset();

    bool should_use_intset() const;

    ObjectEncoding encoding_;
    std::variant<
        intset,                          // 编码: INTSET
        std::unordered_set<std::string>  // 编码: STRSET
    > storage_;

    // 辅助访问
    intset& get_intset() { return std::get<0>(storage_); }
    std::unordered_set<std::string>& get_strset() { return std::get<1>(storage_); }
};