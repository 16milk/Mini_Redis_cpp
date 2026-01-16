#pragma once
#include "RedisObject.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <variant>
#include <vector>
#include <optional>
#include <utility> // for pair

enum class ObjectEncoding {
    ZIPLIST,   // 小列表：用 vector 模拟
    LINKEDLIST // 大列表：用 deque
};

// 列表对象（元素为二进制安全的字节序列，用 std::string 表示）
class ListObject : public RedisObject {
public:
    static constexpr size_t ZIPLIST_MAX_ENTRIES = 512;
    static constexpr size_t ZIPLIST_MAX_ENTRY_SIZE = 64; // 单个元素最大 64 字节

    ListObject();

    void push_front(std::string value);
    void push_back(std::string value);
    bool pop_front(std::string& out);
    bool pop_back(std::string& out);
    bool get(int index, std::string& out) const;
    long long remove(long long count, const std::string& value);
    void trim(int start, int stop);
    bool insert(const std::string& pivot, const std::string& value, InsertPosition pos);
    size_t size() const;

    // 用于调试：查看当前编码
    ObjectEncoding encoding() const { return encoding_; }

private:
    ObjectEncoding encoding_;
    std::variant<
        std::vector<std::string>,   // ziplist 模拟
        std::deque<std::string>     // linkedlist 模拟
    > storage_;

    // 辅助：检查是否应保持 ziplist
    bool should_use_ziplist() const;

    // 升级：从 ziplist → linkedlist
    void promote_to_linkedlist();

    // 获取可变引用（用于修改）
    std::vector<std::string>& get_ziplist();
    std::deque<std::string>& get_linkedlist();

    // 获取只读引用
    const std::vector<std::string>& get_ziplist() const;
    const std::deque<std::string>& get_linkedlist() const;

    // 索引归一化（复用）
    std::optional<size_t> normalize_index(int index, size_t size) const;
    std::optional<std::pair<size_t, size_t>> normalize_range(int start, int stop, size_t sz) const;
};