#include "RedisObject.hpp"

// RedisObject 的实现
RedisObject::RedisObject(ObjectType type) : type_(type) {}

ObjectType RedisObject::type() const {
    return type_;
}
