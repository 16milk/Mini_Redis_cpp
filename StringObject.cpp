#include "StringObject.hpp"

// StringObject 的实现
StringObject::StringObject(std::string value)
    : RedisObject(ObjectType::STRING), value_(std::move(value)) {}


void StringObject::set_value(std::string v) {
    value_ = std::move(v);
}

size_t StringObject::memory_usage() const {
    return sizeof(*this) + value_.capacity(); // 近似
}