#pragma once
#include "RedisObject.hpp"
#include <string>

// 字符串对象
class StringObject : public RedisObject {
public:
    explicit StringObject(std::string value);
    
    const std::string& value() const;
    void set_value(std::string v);

private:
    std::string value_;
};