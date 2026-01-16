#pragma once
#include <string>
#include <unordered_map>
#include <memory>

enum class ObjectType {
    STRING,
    HASH,
    // LIST, SET, ZSET 可后续添加
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

// 字符串对象
class StringObject : public RedisObject {
public:
    explicit StringObject(std::string value);
    
    const std::string& value() const;
    void set_value(std::string v);

private:
    std::string value_;
};

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