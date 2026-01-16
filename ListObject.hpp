#pragma once
#include "RedisObject.hpp"
#include <string>
#include <unordered_map>
#include <memory>

// 列表对象
class ListObject : public RedisObject {
public:
    ListObject();

    void push_front(const std::string& value);
    void push_back(const std::string& value);
    bool pop_front(std::string& out);
    bool pop_back(std::string& out);
    bool get(size_t index, std::string& out) const;
    size_t size() const;
    // 可选：insert, remove, trim 等

private:
    std::deque<std:: string> list_; // deque 支持 O(1) 首尾操作 + O(1) 随机访问
};