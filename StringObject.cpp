#include "StringObject.hpp"

// StringObject 的实现
StringObject::StringObject(std::string value)
    : RedisObject(ObjectType::STRING), value_(std::move(value)) {}

const std::string& StringObject::value() const {
    return value_;
}

void StringObject::set_value(std::string v) {
    value_ = std::move(v);
}