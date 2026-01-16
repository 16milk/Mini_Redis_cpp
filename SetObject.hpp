#pragma once
#include "RedisObject.hpp"
#include <string>
#include <unordered_map>
#include <memory>

// 集合对象
class SetObject : public RedisObject {
public:
    SetObject();

    bool add(const std::string& member);
    bool remove(const std::string& member);
    bool contains(const std::string& member) const;
    size_t size() const;
    const std::unordered_set<std::string>& members() const;

private:
    std::unordered_set<std::string> members_;
};