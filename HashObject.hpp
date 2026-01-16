#pragma once
#include "RedisObject.hpp"
#include <string>
#include <unordered_map>

// 哈希对象
class HashObject : public RedisObject {
public:
    HashObject();
    
    void set_field(const std::string& field, const std::string& value);
    bool get_field(const std::string& field, std::string& out_value) const;
    const std::unordered_map<std::string, std::string>& getFields() const;
    size_t size() const;

private:
    std::unordered_map<std::string, std::string> fields_;
};