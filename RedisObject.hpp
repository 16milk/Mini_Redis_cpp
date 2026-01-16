#pragma once
#include <string>
#include <unordered_map>
#include <memory>

enum class ObjectType {
    STRING,
    LIST,
    SET,
    HASH,
    ZSET
};

class RedisObject {
public:
    explicit RedisObject(ObjectType type);
    virtual ~RedisObject() = default;

    ObjectType type() const;

    // 类型安全的 downcast 工具（可选）
    template<typename T>
    T* as() { return dynamic_cast<T*>(this); }

private:
    ObjectType type_;
};
