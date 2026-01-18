#pragma once
#include "RedisObject.hpp"
#include <string>

// 字符串对象
class StringObject : public RedisObject {
public:
    explicit StringObject(std::string value);
    
    ObjectType type() const override { return ObjectType::STRING; }
    size_t memory_usage() const override;

    void set_value(std::string v);  

    const std::string& value() const { return value_; }

private:
    std::string value_;
};